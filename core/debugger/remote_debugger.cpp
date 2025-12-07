/**************************************************************************/
/*  remote_debugger.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "remote_debugger.h"

#include "core/config/project_settings.h"
#include "core/debugger/debugger_marshalls.h"
#include "core/debugger/engine_debugger.h"
#include "core/debugger/engine_profiler.h"
#include "core/debugger/script_debugger.h"
#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/io/resource_loader.h"
#include "core/math/expression.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/display/display_server.h"

#ifndef _2D_DISABLED
#include "scene/2d/node_2d.h"
#endif
#ifndef _3D_DISABLED
#include "scene/3d/node_3d.h"
#endif
#ifndef ADVANCED_GUI_DISABLED
#include "scene/gui/control.h"
#endif

class RemoteDebugger::PerformanceProfiler : public EngineProfiler {
	Object *performance = nullptr;
	int last_perf_time = 0;
	uint64_t last_monitor_modification_time = 0;

public:
	void toggle(bool p_enable, const Array &p_opts) override {}
	void add(const Array &p_data) override {}
	void tick(double p_frame_time, double p_process_time, double p_physics_time, double p_physics_frame_time) override {
		if (!performance) {
			return;
		}

		uint64_t pt = OS::get_singleton()->get_ticks_msec();
		if (pt - last_perf_time < 1000) {
			return;
		}
		last_perf_time = pt;

		Array custom_monitor_names = performance->call("get_custom_monitor_names");
		Array custom_monitor_types = performance->call("get_custom_monitor_types");

		Array custom_monitor_data;
		custom_monitor_data.push_back(custom_monitor_names);
		custom_monitor_data.push_back(custom_monitor_types);

		uint64_t monitor_modification_time = performance->call("get_monitor_modification_time");
		if (monitor_modification_time > last_monitor_modification_time) {
			last_monitor_modification_time = monitor_modification_time;
			EngineDebugger::get_singleton()->send_message("performance:profile_names", custom_monitor_data);
		}

		int max = performance->get("MONITOR_MAX");
		Array arr;
		arr.resize(max + custom_monitor_names.size());
		for (int i = 0; i < max; i++) {
			arr[i] = performance->call("get_monitor", i);
		}

		for (int i = 0; i < custom_monitor_names.size(); i++) {
			Variant monitor_value = performance->call("get_custom_monitor", custom_monitor_names[i]);
			if (!monitor_value.is_num()) {
				ERR_PRINT(vformat("Value of custom monitor '%s' is not a number.", String(custom_monitor_names[i])));
				arr[i + max] = Variant();
			} else {
				arr[i + max] = monitor_value;
			}
		}

		EngineDebugger::get_singleton()->send_message("performance:profile_frame", arr);
	}

	explicit PerformanceProfiler(Object *p_performance) {
		performance = p_performance;
	}
};

Error RemoteDebugger::_put_msg(const String &p_message, const Array &p_data) {
	Array msg = { p_message, Thread::get_caller_id(), p_data };
	Error err = peer->put_message(msg);
	if (err != OK) {
		n_messages_dropped++;
	}
	return err;
}

void RemoteDebugger::_err_handler(void *p_this, const char *p_func, const char *p_file, int p_line, const char *p_err, const char *p_descr, bool p_editor_notify, ErrorHandlerType p_type) {
	RemoteDebugger *rd = static_cast<RemoteDebugger *>(p_this);
	if (rd->flushing && Thread::get_caller_id() == rd->flush_thread) { // Can't handle recursive errors during flush.
		return;
	}

	Vector<ScriptLanguage::StackInfo> si;

	for (int i = 0; i < ScriptServer::get_language_count(); i++) {
		si = ScriptServer::get_language(i)->debug_get_current_stack_info();
		if (si.size()) {
			break;
		}
	}

	// send_error will lock internally.
	rd->script_debugger->send_error(String::utf8(p_func), String::utf8(p_file), p_line, String::utf8(p_err), String::utf8(p_descr), p_editor_notify, p_type, si);
}

void RemoteDebugger::_print_handler(void *p_this, const String &p_string, bool p_error, bool p_rich) {
	RemoteDebugger *rd = static_cast<RemoteDebugger *>(p_this);

	if (rd->flushing && Thread::get_caller_id() == rd->flush_thread) { // Can't handle recursive prints during flush.
		return;
	}

	String s = p_string;
	int allowed_chars = MIN(MAX(rd->max_chars_per_second - rd->char_count, 0), s.length());

	if (allowed_chars == 0 && s.length() > 0) {
		return;
	}

	if (allowed_chars < s.length()) {
		s = s.substr(0, allowed_chars);
	}

	MutexLock lock(rd->mutex);

	rd->char_count += allowed_chars;
	bool overflowed = rd->char_count >= rd->max_chars_per_second;
	if (rd->is_peer_connected()) {
		if (overflowed) {
			s += "[...]";
		}

		OutputString output_string;
		output_string.message = s;
		if (p_error) {
			output_string.type = MESSAGE_TYPE_ERROR;
		} else if (p_rich) {
			output_string.type = MESSAGE_TYPE_LOG_RICH;
		} else {
			output_string.type = MESSAGE_TYPE_LOG;
		}
		rd->output_strings.push_back(output_string);

		if (overflowed) {
			output_string.message = "[output overflow, print less text!]";
			output_string.type = MESSAGE_TYPE_ERROR;
			rd->output_strings.push_back(output_string);
		}
	}
}

RemoteDebugger::ErrorMessage RemoteDebugger::_create_overflow_error(const String &p_what, const String &p_descr) {
	ErrorMessage oe;
	oe.error = p_what;
	oe.error_descr = p_descr;
	oe.warning = false;
	uint64_t time = OS::get_singleton()->get_ticks_msec();
	oe.hr = time / 3600000;
	oe.min = (time / 60000) % 60;
	oe.sec = (time / 1000) % 60;
	oe.msec = time % 1000;
	return oe;
}

void RemoteDebugger::flush_output() {
	MutexLock lock(mutex);
	flush_thread = Thread::get_caller_id();
	flushing = true;
	if (!is_peer_connected()) {
		return;
	}

	if (n_messages_dropped > 0) {
		ErrorMessage err_msg = _create_overflow_error("TOO_MANY_MESSAGES", "Too many messages! " + String::num_int64(n_messages_dropped) + " messages were dropped. Profiling might misbheave, try raising 'network/limits/debugger/max_queued_messages' in project setting.");
		if (_put_msg("error", err_msg.serialize()) == OK) {
			n_messages_dropped = 0;
		}
	}

	if (output_strings.size()) {
		// Join output strings so we generate less messages.
		Vector<String> joined_log_strings;
		Vector<String> strings;
		Vector<int> types;
		for (const OutputString &output_string : output_strings) {
			if (output_string.type == MESSAGE_TYPE_ERROR) {
				if (!joined_log_strings.is_empty()) {
					strings.push_back(String("\n").join(joined_log_strings));
					types.push_back(MESSAGE_TYPE_LOG);
					joined_log_strings.clear();
				}
				strings.push_back(output_string.message);
				types.push_back(MESSAGE_TYPE_ERROR);
			} else if (output_string.type == MESSAGE_TYPE_LOG_RICH) {
				if (!joined_log_strings.is_empty()) {
					strings.push_back(String("\n").join(joined_log_strings));
					types.push_back(MESSAGE_TYPE_LOG_RICH);
					joined_log_strings.clear();
				}
				strings.push_back(output_string.message);
				types.push_back(MESSAGE_TYPE_LOG_RICH);
			} else {
				joined_log_strings.push_back(output_string.message);
			}
		}

		if (!joined_log_strings.is_empty()) {
			strings.push_back(String("\n").join(joined_log_strings));
			types.push_back(MESSAGE_TYPE_LOG);
		}

		Array arr = { strings, types };
		_put_msg("output", arr);
		output_strings.clear();
	}

	while (errors.size()) {
		ErrorMessage oe = errors.front()->get();
		_put_msg("error", oe.serialize());
		errors.pop_front();
	}

	// Update limits
	uint64_t ticks = OS::get_singleton()->get_ticks_usec() / 1000;

	if (ticks - last_reset > 1000) {
		last_reset = ticks;
		char_count = 0;
		err_count = 0;
		n_errors_dropped = 0;
		warn_count = 0;
		n_warnings_dropped = 0;
	}
	flushing = false;
}

void RemoteDebugger::send_message(const String &p_message, const Array &p_args) {
	MutexLock lock(mutex);
	if (is_peer_connected()) {
		_put_msg(p_message, p_args);
	}
}

void RemoteDebugger::send_error(const String &p_func, const String &p_file, int p_line, const String &p_err, const String &p_descr, bool p_editor_notify, ErrorHandlerType p_type) {
	ErrorMessage oe;
	oe.error = p_err;
	oe.error_descr = p_descr;
	oe.source_file = p_file;
	oe.source_line = p_line;
	oe.source_func = p_func;
	oe.warning = p_type == ERR_HANDLER_WARNING;
	uint64_t time = OS::get_singleton()->get_ticks_msec();
	oe.hr = time / 3600000;
	oe.min = (time / 60000) % 60;
	oe.sec = (time / 1000) % 60;
	oe.msec = time % 1000;
	oe.callstack.append_array(script_debugger->get_error_stack_info());

	if (flushing && Thread::get_caller_id() == flush_thread) { // Can't handle recursive errors during flush.
		return;
	}

	MutexLock lock(mutex);

	if (oe.warning) {
		warn_count++;
	} else {
		err_count++;
	}

	if (is_peer_connected()) {
		if (oe.warning) {
			if (warn_count > max_warnings_per_second) {
				n_warnings_dropped++;
				if (n_warnings_dropped == 1) {
					// Only print one message about dropping per second
					ErrorMessage overflow = _create_overflow_error("TOO_MANY_WARNINGS", "Too many warnings! Ignoring warnings for up to 1 second.");
					errors.push_back(overflow);
				}
			} else {
				errors.push_back(oe);
			}
		} else {
			if (err_count > max_errors_per_second) {
				n_errors_dropped++;
				if (n_errors_dropped == 1) {
					// Only print one message about dropping per second
					ErrorMessage overflow = _create_overflow_error("TOO_MANY_ERRORS", "Too many errors! Ignoring errors for up to 1 second.");
					errors.push_back(overflow);
				}
			} else {
				errors.push_back(oe);
			}
		}
	}
}

void RemoteDebugger::_send_stack_vars(List<String> &p_names, List<Variant> &p_vals, int p_type) {
	DebuggerMarshalls::ScriptStackVariable stvar;
	List<String>::Element *E = p_names.front();
	List<Variant>::Element *F = p_vals.front();
	while (E) {
		stvar.name = E->get();
		stvar.value = F->get();
		stvar.type = p_type;
		send_message("stack_frame_var", stvar.serialize());
		E = E->next();
		F = F->next();
	}
}

Error RemoteDebugger::_try_capture(const String &p_msg, const Array &p_data, bool &r_captured) {
	const int idx = p_msg.find_char(':');
	r_captured = false;
	if (idx < 0) { // No prefix, unknown message.
		return OK;
	}
	const String cap = p_msg.substr(0, idx);
	if (!has_capture(cap)) {
		return ERR_UNAVAILABLE; // Unknown message...
	}
	const String msg = p_msg.substr(idx + 1);
	return capture_parse(cap, msg, p_data, r_captured);
}

void RemoteDebugger::_poll_messages() {
	MutexLock mutex_lock(mutex);

	peer->poll();
	while (peer->has_message()) {
		Array cmd = peer->get_message();
		ERR_CONTINUE(cmd.size() != 3);
		ERR_CONTINUE(cmd[0].get_type() != Variant::STRING);
		ERR_CONTINUE(cmd[1].get_type() != Variant::INT);
		ERR_CONTINUE(cmd[2].get_type() != Variant::ARRAY);

		Thread::ID thread = cmd[1];

		if (!messages.has(thread)) {
			continue; // This thread is not around to receive the messages
		}

		Message msg;
		msg.message = cmd[0];
		msg.data = cmd[2];
		messages[thread].push_back(msg);
	}
}

bool RemoteDebugger::_has_messages() {
	MutexLock mutex_lock(mutex);
	return messages.has(Thread::get_caller_id()) && !messages[Thread::get_caller_id()].is_empty();
}

Array RemoteDebugger::_get_message() {
	MutexLock mutex_lock(mutex);
	ERR_FAIL_COND_V(!messages.has(Thread::get_caller_id()), Array());
	List<Message> &message_list = messages[Thread::get_caller_id()];
	ERR_FAIL_COND_V(message_list.is_empty(), Array());

	Array msg;
	msg.resize(2);
	msg[0] = message_list.front()->get().message;
	msg[1] = message_list.front()->get().data;
	message_list.pop_front();
	return msg;
}

void RemoteDebugger::debug(bool p_can_continue, bool p_is_error_breakpoint) {
	//this function is called when there is a debugger break (bug on script)
	//or when execution is paused from editor

	{
		MutexLock lock(mutex);
		// Tests that require mutex.
		if (script_debugger->is_skipping_breakpoints() && !p_is_error_breakpoint) {
			return;
		}

		ERR_FAIL_COND_MSG(!is_peer_connected(), "Script Debugger failed to connect, but being used anyway.");

		if (!peer->can_block()) {
			return; // Peer does not support blocking IO. We could at least send the error though.
		}
	}

	if (p_is_error_breakpoint && script_debugger->is_ignoring_error_breaks()) {
		return;
	}

	ScriptLanguage *script_lang = script_debugger->get_break_language();
	ERR_FAIL_NULL(script_lang);

	Array msg = {
		p_can_continue,
		script_lang->debug_get_error(),
		script_lang->debug_get_stack_level_count() > 0,
		Thread::get_caller_id()
	};
	if (allow_focus_steal_fn) {
		allow_focus_steal_fn();
	}
	send_message("debug_enter", msg);

	Input::MouseMode mouse_mode = Input::MOUSE_MODE_VISIBLE;

	if (Thread::get_caller_id() == Thread::get_main_id()) {
		mouse_mode = Input::get_singleton()->get_mouse_mode();
		if (mouse_mode != Input::MOUSE_MODE_VISIBLE) {
			Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
		}
	} else {
		MutexLock mutex_lock(mutex);
		messages.insert(Thread::get_caller_id(), List<Message>());
	}

	while (is_peer_connected()) {
		flush_output();

		_poll_messages();

		if (_has_messages()) {
			Array cmd = _get_message();

			ERR_CONTINUE(cmd.size() != 2);
			ERR_CONTINUE(cmd[0].get_type() != Variant::STRING);
			ERR_CONTINUE(cmd[1].get_type() != Variant::ARRAY);

			String command = cmd[0];
			Array data = cmd[1];

			if (command == "step") {
				script_debugger->set_depth(-1);
				script_debugger->set_lines_left(1);
				break;

			} else if (command == "next") {
				script_debugger->set_depth(0);
				script_debugger->set_lines_left(1);
				break;

			} else if (command == "out") {
				script_debugger->set_depth(1);
				script_debugger->set_lines_left(1);
				break;

			} else if (command == "continue") {
				script_debugger->set_depth(-1);
				script_debugger->set_lines_left(-1);
				break;

			} else if (command == "break") {
				ERR_PRINT("Got break when already broke!");
				break;

			} else if (command == "get_stack_dump") {
				DebuggerMarshalls::ScriptStackDump dump;
				int slc = script_lang->debug_get_stack_level_count();
				for (int i = 0; i < slc; i++) {
					ScriptLanguage::StackInfo frame;
					frame.file = script_lang->debug_get_stack_level_source(i);
					frame.line = script_lang->debug_get_stack_level_line(i);
					frame.func = script_lang->debug_get_stack_level_function(i);
					dump.frames.push_back(frame);
				}
				send_message("stack_dump", dump.serialize());

			} else if (command == "get_stack_frame_vars") {
				ERR_FAIL_COND(data.size() != 1);
				ERR_FAIL_NULL(script_lang);
				int lv = data[0];

				List<String> members;
				List<Variant> member_vals;
				if (ScriptInstance *inst = script_lang->debug_get_stack_level_instance(lv)) {
					members.push_back("self");
					member_vals.push_back(inst->get_owner());
				}
				script_lang->debug_get_stack_level_members(lv, &members, &member_vals);
				ERR_FAIL_COND(members.size() != member_vals.size());

				List<String> locals;
				List<Variant> local_vals;
				script_lang->debug_get_stack_level_locals(lv, &locals, &local_vals);
				ERR_FAIL_COND(locals.size() != local_vals.size());

				List<String> globals;
				List<Variant> globals_vals;
				script_lang->debug_get_globals(&globals, &globals_vals);
				ERR_FAIL_COND(globals.size() != globals_vals.size());

				Array var_size = { local_vals.size() + member_vals.size() + globals_vals.size() };
				send_message("stack_frame_vars", var_size);
				_send_stack_vars(locals, local_vals, 0);
				_send_stack_vars(members, member_vals, 1);
				_send_stack_vars(globals, globals_vals, 2);

			} else if (command == "reload_scripts") {
				script_paths_to_reload = data;
			} else if (command == "reload_all_scripts") {
				reload_all_scripts = true;
			} else if (command == "breakpoint") {
				ERR_FAIL_COND(data.size() < 3);
				bool set = data[2];
				if (set) {
					script_debugger->insert_breakpoint(data[1], data[0]);
				} else {
					script_debugger->remove_breakpoint(data[1], data[0]);
				}

			} else if (command == "set_skip_breakpoints") {
				ERR_FAIL_COND(data.is_empty());
				script_debugger->set_skip_breakpoints(data[0]);
			} else if (command == "set_ignore_error_breaks") {
				ERR_FAIL_COND(data.is_empty());
				script_debugger->set_ignore_error_breaks(data[0]);
			} else if (command == "evaluate") {
				String expression_str = data[0];
				int frame = data[1];

				ScriptInstance *breaked_instance = script_debugger->get_break_language()->debug_get_stack_level_instance(frame);
				if (!breaked_instance) {
					break;
				}

				PackedStringArray input_names;
				Array input_vals;

				List<String> locals;
				List<Variant> local_vals;
				script_debugger->get_break_language()->debug_get_stack_level_locals(frame, &locals, &local_vals);
				ERR_FAIL_COND(locals.size() != local_vals.size());

				for (const String &S : locals) {
					input_names.append(S);
				}

				for (const Variant &V : local_vals) {
					input_vals.append(V);
				}

				List<String> globals;
				List<Variant> globals_vals;
				script_debugger->get_break_language()->debug_get_globals(&globals, &globals_vals);
				ERR_FAIL_COND(globals.size() != globals_vals.size());

				for (const String &S : globals) {
					input_names.append(S);
				}

				for (const Variant &V : globals_vals) {
					input_vals.append(V);
				}

				LocalVector<StringName> native_types;
				ClassDB::get_class_list(native_types);
				for (const StringName &class_name : native_types) {
					if (!ClassDB::is_class_exposed(class_name) || !Engine::get_singleton()->has_singleton(class_name) || Engine::get_singleton()->is_singleton_editor_only(class_name)) {
						continue;
					}

					input_names.append(class_name);
					input_vals.append(Engine::get_singleton()->get_singleton_object(class_name));
				}

				LocalVector<StringName> user_types;
				ScriptServer::get_global_class_list(user_types);
				for (const StringName &class_name : user_types) {
					String scr_path = ScriptServer::get_global_class_path(class_name);
					Ref<Script> scr = ResourceLoader::load(scr_path, "Script");
					ERR_CONTINUE_MSG(scr.is_null(), vformat(R"(Could not load the global class %s from resource path: "%s".)", class_name, scr_path));

					input_names.append(class_name);
					input_vals.append(scr);
				}

				Expression expression;
				expression.parse(expression_str, input_names);
				const Variant return_val = expression.execute(input_vals, breaked_instance->get_owner());

				DebuggerMarshalls::ScriptStackVariable stvar;
				stvar.name = expression_str;
				stvar.value = return_val;
				stvar.type = 3;

				send_message("evaluation_return", stvar.serialize());
			} else {
				bool captured = false;
				ERR_CONTINUE(_try_capture(command, data, captured) != OK);
				if (!captured) {
					WARN_PRINT(vformat("Unknown message received from debugger: %s.", command));
				}
			}
		} else {
			OS::get_singleton()->delay_usec(10000);
			if (Thread::get_caller_id() == Thread::get_main_id()) {
				// If this is a busy loop on the main thread, events still need to be processed.
				DisplayServer::get_singleton()->force_process_and_drop_events();
			}
		}
	}

	send_message("debug_exit", Array());

	if (Thread::get_caller_id() == Thread::get_main_id()) {
		if (mouse_mode != Input::MOUSE_MODE_VISIBLE) {
			Input::get_singleton()->set_mouse_mode(mouse_mode);
		}
	} else {
		MutexLock mutex_lock(mutex);
		messages.erase(Thread::get_caller_id());
	}
}

void RemoteDebugger::poll_events(bool p_is_idle) {
	if (peer.is_null()) {
		return;
	}

	flush_output();

	_poll_messages();

	while (_has_messages()) {
		Array arr = _get_message();

		ERR_CONTINUE(arr.size() != 2);
		ERR_CONTINUE(arr[0].get_type() != Variant::STRING);
		ERR_CONTINUE(arr[1].get_type() != Variant::ARRAY);

		const String cmd = arr[0];
		const int idx = cmd.find_char(':');
		bool parsed = false;
		if (idx < 0) { // Not prefix, use scripts capture.
			capture_parse("core", cmd, arr[1], parsed);
			continue;
		}

		const String cap = cmd.substr(0, idx);
		if (!has_capture(cap)) {
			continue; // Unknown message...
		}

		const String msg = cmd.substr(idx + 1);
		capture_parse(cap, msg, arr[1], parsed);
	}

	// Reload scripts during idle poll only.
	if (p_is_idle) {
		if (reload_all_scripts) {
			for (int i = 0; i < ScriptServer::get_language_count(); i++) {
				ScriptServer::get_language(i)->reload_all_scripts();
			}
			reload_all_scripts = false;
		} else if (!script_paths_to_reload.is_empty()) {
			Array scripts_to_reload;
			for (const Variant &v : script_paths_to_reload) {
				const String &path = v;
				Error err = OK;
				Ref<Script> script = ResourceLoader::load(path, "", ResourceFormatLoader::CACHE_MODE_REUSE, &err);
				ERR_CONTINUE_MSG(err != OK, vformat("Could not reload script '%s': %s", path, error_names[err]));
				ERR_CONTINUE_MSG(script.is_null(), vformat("Could not reload script '%s': Not a script!", path, error_names[err]));
				scripts_to_reload.push_back(script);
			}
			for (int i = 0; i < ScriptServer::get_language_count(); i++) {
				ScriptServer::get_language(i)->reload_scripts(scripts_to_reload, true);
			}
		}
		script_paths_to_reload.clear();
	}
}

Error RemoteDebugger::_core_capture(const String &p_cmd, const Array &p_data, bool &r_captured) {
	r_captured = true;
	if (p_cmd == "reload_scripts") {
		script_paths_to_reload = p_data;
	} else if (p_cmd == "reload_all_scripts") {
		reload_all_scripts = true;
	} else if (p_cmd == "breakpoint") {
		ERR_FAIL_COND_V(p_data.size() < 3, ERR_INVALID_DATA);
		bool set = p_data[2];
		if (set) {
			script_debugger->insert_breakpoint(p_data[1], p_data[0]);
		} else {
			script_debugger->remove_breakpoint(p_data[1], p_data[0]);
		}

	} else if (p_cmd == "set_skip_breakpoints") {
		ERR_FAIL_COND_V(p_data.is_empty(), ERR_INVALID_DATA);
		script_debugger->set_skip_breakpoints(p_data[0]);
	} else if (p_cmd == "set_ignore_error_breaks") {
		ERR_FAIL_COND_V(p_data.is_empty(), ERR_INVALID_DATA);
		script_debugger->set_ignore_error_breaks(p_data[0]);
	} else if (p_cmd == "break") {
		script_debugger->debug(script_debugger->get_break_language());
	} else {
		r_captured = false;
	}
	return OK;
}

Error RemoteDebugger::_profiler_capture(const String &p_cmd, const Array &p_data, bool &r_captured) {
	r_captured = false;
	ERR_FAIL_COND_V(p_data.is_empty(), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(p_data[0].get_type() != Variant::BOOL, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(!has_profiler(p_cmd), ERR_UNAVAILABLE);
	Array opts;
	if (p_data.size() > 1) { // Optional profiler parameters.
		ERR_FAIL_COND_V(p_data[1].get_type() != Variant::ARRAY, ERR_INVALID_DATA);
		opts = p_data[1];
	}
	r_captured = true;
	profiler_enable(p_cmd, p_data[0], opts);
	return OK;
}

Error RemoteDebugger::_automation_capture(const String &p_cmd, const Array &p_data, bool &r_captured) {
	r_captured = true;
	if (p_cmd == "get_tree") {
		_send_scene_tree();
	} else if (p_cmd == "get_node") {
		ERR_FAIL_COND_V(p_data.size() < 1, ERR_INVALID_DATA);
		_send_node_info(p_data[0]);
	} else if (p_cmd == "get_property") {
		ERR_FAIL_COND_V(p_data.size() < 2, ERR_INVALID_DATA);
		_send_property(p_data[0], p_data[1]);
	} else if (p_cmd == "set_property") {
		ERR_FAIL_COND_V(p_data.size() < 3, ERR_INVALID_DATA);
		_set_property(p_data[0], p_data[1], p_data[2]);
	} else if (p_cmd == "call_method") {
		ERR_FAIL_COND_V(p_data.size() < 2, ERR_INVALID_DATA);
		Array args = p_data.size() > 2 ? Array(p_data[2]) : Array();
		_call_method(p_data[0], p_data[1], args);
	} else if (p_cmd == "mouse_button") {
		// mouse_button: [x, y, button_index, pressed, double_click?]
		ERR_FAIL_COND_V(p_data.size() < 4, ERR_INVALID_DATA);
		Vector2 pos(p_data[0], p_data[1]);
		bool double_click = p_data.size() > 4 ? (bool)p_data[4] : false;
		_inject_mouse_button(pos, p_data[2], p_data[3], double_click);
	} else if (p_cmd == "mouse_motion") {
		// mouse_motion: [x, y, relative_x, relative_y]
		ERR_FAIL_COND_V(p_data.size() < 4, ERR_INVALID_DATA);
		Vector2 pos(p_data[0], p_data[1]);
		Vector2 rel(p_data[2], p_data[3]);
		_inject_mouse_motion(pos, rel);
	} else if (p_cmd == "key") {
		// key: [keycode, pressed, physical?]
		ERR_FAIL_COND_V(p_data.size() < 2, ERR_INVALID_DATA);
		bool physical = p_data.size() > 2 ? (bool)p_data[2] : false;
		_inject_key(p_data[0], p_data[1], physical);
	} else if (p_cmd == "touch") {
		// touch: [index, x, y, pressed]
		ERR_FAIL_COND_V(p_data.size() < 4, ERR_INVALID_DATA);
		Vector2 pos(p_data[1], p_data[2]);
		_inject_touch(p_data[0], pos, p_data[3]);
	} else if (p_cmd == "action") {
		// action: [action_name, pressed, strength?]
		ERR_FAIL_COND_V(p_data.size() < 2, ERR_INVALID_DATA);
		float strength = p_data.size() > 2 ? (float)p_data[2] : 1.0f;
		_inject_action(p_data[0], p_data[1], strength);
	} else if (p_cmd == "screenshot") {
		// screenshot: [] or [node_path]
		String node_path = p_data.size() > 0 ? String(p_data[0]) : "";
		_send_screenshot(node_path);
	} else if (p_cmd == "query_nodes") {
		// query_nodes: [pattern]
		ERR_FAIL_COND_V(p_data.size() < 1, ERR_INVALID_DATA);
		_query_nodes(p_data[0]);
	} else if (p_cmd == "count_nodes") {
		// count_nodes: [pattern]
		ERR_FAIL_COND_V(p_data.size() < 1, ERR_INVALID_DATA);
		_count_nodes(p_data[0]);
	} else if (p_cmd == "get_current_scene") {
		_send_current_scene();
	} else if (p_cmd == "change_scene") {
		// change_scene: [scene_path]
		ERR_FAIL_COND_V(p_data.size() < 1, ERR_INVALID_DATA);
		_change_scene(p_data[0]);
	} else if (p_cmd == "reload_scene") {
		_reload_scene();
	} else if (p_cmd == "pause") {
		// pause: [paused]
		ERR_FAIL_COND_V(p_data.size() < 1, ERR_INVALID_DATA);
		_set_pause(p_data[0]);
	} else if (p_cmd == "time_scale") {
		// time_scale: [scale]
		ERR_FAIL_COND_V(p_data.size() < 1, ERR_INVALID_DATA);
		_set_time_scale(p_data[0]);
	} else {
		r_captured = false;
	}
	return OK;
}

void RemoteDebugger::_send_scene_tree() {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Node *root = tree->get_root();
	ERR_FAIL_NULL(root);

	Dictionary tree_data = _serialize_node(root);

	Array msg;
	msg.push_back(tree_data);
	EngineDebugger::get_singleton()->send_message("automation:tree", msg);
}

void RemoteDebugger::_send_node_info(const String &p_path) {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Node *node = tree->get_root()->get_node_or_null(NodePath(p_path));

	Array msg;
	if (node) {
		msg.push_back(_serialize_node(node));
	} else {
		msg.push_back(Variant());
	}
	EngineDebugger::get_singleton()->send_message("automation:node", msg);
}

void RemoteDebugger::_send_property(const String &p_path, const String &p_property) {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Node *node = tree->get_root()->get_node_or_null(NodePath(p_path));

	Array msg;
	msg.push_back(p_path);
	msg.push_back(p_property);

	if (node) {
		msg.push_back(node->get(p_property));
	} else {
		msg.push_back(Variant());
	}
	EngineDebugger::get_singleton()->send_message("automation:property", msg);
}

void RemoteDebugger::_set_property(const String &p_path, const String &p_property, const Variant &p_value) {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Node *node = tree->get_root()->get_node_or_null(NodePath(p_path));

	bool success = false;
	if (node) {
		node->set(p_property, p_value);
		success = true;
	}

	Array msg;
	msg.push_back(success);
	EngineDebugger::get_singleton()->send_message("automation:set_result", msg);
}

void RemoteDebugger::_call_method(const String &p_path, const String &p_method, const Array &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Node *node = tree->get_root()->get_node_or_null(NodePath(p_path));

	Array msg;
	msg.push_back(p_path);
	msg.push_back(p_method);

	if (node && node->has_method(p_method)) {
		Variant result = node->callv(p_method, p_args);
		msg.push_back(result);
	} else {
		msg.push_back(Variant());
	}
	EngineDebugger::get_singleton()->send_message("automation:call_result", msg);
}

Dictionary RemoteDebugger::_serialize_node(Node *p_node) {
	Dictionary data;
	data["name"] = p_node->get_name();
	data["path"] = String(p_node->get_path());
	data["class"] = p_node->get_class();

	// Add position/visibility for common node types
#ifndef _2D_DISABLED
	if (Node2D *n2d = Object::cast_to<Node2D>(p_node)) {
		data["position"] = n2d->get_position();
		data["rotation"] = n2d->get_rotation();
		data["scale"] = n2d->get_scale();
		data["visible"] = n2d->is_visible();
	}
#endif
#ifndef ADVANCED_GUI_DISABLED
	if (Control *ctrl = Object::cast_to<Control>(p_node)) {
		data["position"] = ctrl->get_position();
		data["size"] = ctrl->get_size();
		data["visible"] = ctrl->is_visible();
	}
#endif
#ifndef _3D_DISABLED
	if (Node3D *n3d = Object::cast_to<Node3D>(p_node)) {
		data["position"] = n3d->get_position();
		data["rotation"] = n3d->get_rotation();
		data["scale"] = n3d->get_scale();
		data["visible"] = n3d->is_visible();
	}
#endif

	// Recurse children
	Array children;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		children.push_back(_serialize_node(p_node->get_child(i)));
	}
	data["children"] = children;

	return data;
}

void RemoteDebugger::_inject_mouse_button(const Vector2 &p_position, int p_button, bool p_pressed, bool p_double_click) {
	Input *input = Input::get_singleton();
	ERR_FAIL_NULL(input);

	Ref<InputEventMouseButton> ev;
	ev.instantiate();
	ev->set_device(InputEvent::DEVICE_ID_EMULATION);
	ev->set_position(p_position);
	ev->set_global_position(p_position);
	ev->set_button_index((MouseButton)p_button);
	ev->set_pressed(p_pressed);
	ev->set_double_click(p_double_click);

	input->parse_input_event(ev);

	Array msg;
	msg.push_back(true);
	EngineDebugger::get_singleton()->send_message("automation:input_result", msg);
}

void RemoteDebugger::_inject_mouse_motion(const Vector2 &p_position, const Vector2 &p_relative) {
	Input *input = Input::get_singleton();
	ERR_FAIL_NULL(input);

	Ref<InputEventMouseMotion> ev;
	ev.instantiate();
	ev->set_device(InputEvent::DEVICE_ID_EMULATION);
	ev->set_position(p_position);
	ev->set_global_position(p_position);
	ev->set_relative(p_relative);
	ev->set_button_mask(input->get_mouse_button_mask());

	input->parse_input_event(ev);

	Array msg;
	msg.push_back(true);
	EngineDebugger::get_singleton()->send_message("automation:input_result", msg);
}

void RemoteDebugger::_inject_key(int p_keycode, bool p_pressed, bool p_physical) {
	Input *input = Input::get_singleton();
	ERR_FAIL_NULL(input);

	Ref<InputEventKey> ev;
	ev.instantiate();
	ev->set_device(InputEvent::DEVICE_ID_EMULATION);
	ev->set_pressed(p_pressed);

	if (p_physical) {
		ev->set_physical_keycode((Key)p_keycode);
	} else {
		ev->set_keycode((Key)p_keycode);
	}

	input->parse_input_event(ev);

	Array msg;
	msg.push_back(true);
	EngineDebugger::get_singleton()->send_message("automation:input_result", msg);
}

void RemoteDebugger::_inject_touch(int p_index, const Vector2 &p_position, bool p_pressed) {
	Input *input = Input::get_singleton();
	ERR_FAIL_NULL(input);

	Ref<InputEventScreenTouch> ev;
	ev.instantiate();
	ev->set_device(InputEvent::DEVICE_ID_EMULATION);
	ev->set_index(p_index);
	ev->set_position(p_position);
	ev->set_pressed(p_pressed);

	input->parse_input_event(ev);

	Array msg;
	msg.push_back(true);
	EngineDebugger::get_singleton()->send_message("automation:input_result", msg);
}

void RemoteDebugger::_inject_action(const String &p_action, bool p_pressed, float p_strength) {
	Input *input = Input::get_singleton();
	ERR_FAIL_NULL(input);

	if (p_pressed) {
		input->action_press(p_action, p_strength);
	} else {
		input->action_release(p_action);
	}

	Array msg;
	msg.push_back(true);
	EngineDebugger::get_singleton()->send_message("automation:input_result", msg);
}

void RemoteDebugger::_send_screenshot(const String &p_node_path) {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Ref<Image> image;

	if (p_node_path.is_empty()) {
		// Capture entire viewport
		Viewport *viewport = tree->get_root();
		ERR_FAIL_NULL(viewport);
		image = viewport->get_texture()->get_image();
	} else {
		// Capture specific node's viewport
		Node *node = tree->get_root()->get_node_or_null(NodePath(p_node_path));
		if (node) {
			CanvasItem *ci = Object::cast_to<CanvasItem>(node);
			if (ci) {
				Viewport *viewport = ci->get_viewport();
				if (viewport) {
					image = viewport->get_texture()->get_image();
				}
			}
		}
	}

	Array msg;
	if (image.is_valid()) {
		PackedByteArray png_data = image->save_png_to_buffer();
		msg.push_back(png_data);
	} else {
		msg.push_back(PackedByteArray());
	}
	EngineDebugger::get_singleton()->send_message("automation:screenshot", msg);
}

void RemoteDebugger::_query_nodes_recursive(Node *p_node, const String &p_pattern, Array &r_results) {
	if (!p_node) {
		return;
	}

	String name = p_node->get_name();
	String node_class = p_node->get_class();

	// Match by name pattern (supports * wildcard) or class name
	bool match = false;
	if (p_pattern.begins_with("*") && p_pattern.ends_with("*")) {
		String inner = p_pattern.substr(1, p_pattern.length() - 2);
		match = name.contains(inner) || node_class.contains(inner);
	} else if (p_pattern.begins_with("*")) {
		String suffix = p_pattern.substr(1);
		match = name.ends_with(suffix) || node_class.ends_with(suffix);
	} else if (p_pattern.ends_with("*")) {
		String prefix = p_pattern.substr(0, p_pattern.length() - 1);
		match = name.begins_with(prefix) || node_class.begins_with(prefix);
	} else {
		match = name == p_pattern || node_class == p_pattern;
	}

	if (match) {
		r_results.push_back(_serialize_node(p_node));
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_query_nodes_recursive(p_node->get_child(i), p_pattern, r_results);
	}
}

void RemoteDebugger::_query_nodes(const String &p_pattern) {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Array results;
	_query_nodes_recursive(tree->get_root(), p_pattern, results);

	Array msg;
	msg.push_back(results);
	EngineDebugger::get_singleton()->send_message("automation:query_result", msg);
}

void RemoteDebugger::_count_nodes(const String &p_pattern) {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Array results;
	_query_nodes_recursive(tree->get_root(), p_pattern, results);

	Array msg;
	msg.push_back(results.size());
	EngineDebugger::get_singleton()->send_message("automation:count_result", msg);
}

void RemoteDebugger::_send_current_scene() {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Array msg;
	Node *current = tree->get_current_scene();
	if (current) {
		msg.push_back(current->get_scene_file_path());
	} else {
		msg.push_back("");
	}
	EngineDebugger::get_singleton()->send_message("automation:current_scene", msg);
}

void RemoteDebugger::_change_scene(const String &p_scene_path) {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Error err = tree->change_scene_to_file(p_scene_path);

	Array msg;
	msg.push_back(err == OK);
	EngineDebugger::get_singleton()->send_message("automation:scene_result", msg);
}

void RemoteDebugger::_reload_scene() {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	Error err = tree->reload_current_scene();

	Array msg;
	msg.push_back(err == OK);
	EngineDebugger::get_singleton()->send_message("automation:scene_result", msg);
}

void RemoteDebugger::_set_pause(bool p_paused) {
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL(tree);

	tree->set_pause(p_paused);

	Array msg;
	msg.push_back(true);
	EngineDebugger::get_singleton()->send_message("automation:pause_result", msg);
}

void RemoteDebugger::_set_time_scale(float p_scale) {
	Engine::get_singleton()->set_time_scale(p_scale);

	Array msg;
	msg.push_back(true);
	EngineDebugger::get_singleton()->send_message("automation:time_scale_result", msg);
}

RemoteDebugger::RemoteDebugger(Ref<RemoteDebuggerPeer> p_peer) {
	peer = p_peer;
	max_chars_per_second = GLOBAL_GET("network/limits/debugger/max_chars_per_second");
	max_errors_per_second = GLOBAL_GET("network/limits/debugger/max_errors_per_second");
	max_warnings_per_second = GLOBAL_GET("network/limits/debugger/max_warnings_per_second");

	// Performance Profiler
	Object *perf = Engine::get_singleton()->get_singleton_object("Performance");
	if (perf) {
		performance_profiler.instantiate(perf);
		performance_profiler->bind("performance");
		profiler_enable("performance", true);
	}

	// Core and profiler captures.
	Capture core_cap(this,
			[](void *p_user, const String &p_cmd, const Array &p_data, bool &r_captured) {
				return static_cast<RemoteDebugger *>(p_user)->_core_capture(p_cmd, p_data, r_captured);
			});
	register_message_capture("core", core_cap);
	Capture profiler_cap(this,
			[](void *p_user, const String &p_cmd, const Array &p_data, bool &r_captured) {
				return static_cast<RemoteDebugger *>(p_user)->_profiler_capture(p_cmd, p_data, r_captured);
			});
	register_message_capture("profiler", profiler_cap);

	// Automation capture for external tool control (e.g., PlayGodot)
	Capture automation_cap(this,
			[](void *p_user, const String &p_cmd, const Array &p_data, bool &r_captured) {
				return static_cast<RemoteDebugger *>(p_user)->_automation_capture(p_cmd, p_data, r_captured);
			});
	register_message_capture("automation", automation_cap);

	// Error handlers
	phl.printfunc = _print_handler;
	phl.userdata = this;
	add_print_handler(&phl);

	eh.errfunc = _err_handler;
	eh.userdata = this;
	add_error_handler(&eh);

	messages.insert(Thread::get_main_id(), List<Message>());
}

RemoteDebugger::~RemoteDebugger() {
	remove_print_handler(&phl);
	remove_error_handler(&eh);
}
