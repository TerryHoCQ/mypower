/*
Copyright (C) 2023 pom@vro.life

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#define __USE_GNU 1
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include <iostream>
#include <random>
#include <regex>
#include <stack>

#include <boost/program_options.hpp>

#include "dsl.hpp"
#include "processlist.hpp"
#include "process.hpp"
#include "scan.hpp"
#include "testview.hpp"
#include "tui.hpp"

#define PARSE_ARG(name)                                                                       \
    try {                                                                                     \
        po::store(po::command_line_parser(arguments)                                          \
                      .options(_options_##name)                                               \
                      .positional(_posiginal_##name)                                          \
                      .run(),                                                                 \
            vm);                                                                              \
        notify(vm);                                                                           \
    } catch (const std::exception& exc) {                                                     \
        _message_view->stream()                                                               \
            << EnableStyle(AttrUnderline) << SetColor(ColorError) << "Error:" << ResetStyle() \
            << " " << exc.what();                                                             \
        return;                                                                               \
    } catch (...) {                                                                           \
        _message_view->stream()                                                               \
            << EnableStyle(AttrUnderline) << SetColor(ColorError) << "Error:" << ResetStyle() \
            << " Unknown error";                                                              \
        return;                                                                               \
    }

namespace po = boost::program_options;

using namespace std::string_literals;
using namespace mypower;
using namespace tui;

class App : public CommandHandler, public std::enable_shared_from_this<App> {
    TUI& _tui;
    std::shared_ptr<MessageView> _message_view;
    std::shared_ptr<HistoryView> _history_view;
    std::shared_ptr<TestView> _test_view;
    std::stack<std::shared_ptr<ContentProvider>> _view_stack {};

    std::shared_ptr<Process> _process;

    std::vector<std::shared_ptr<SessionView>> _session_views {};
    std::shared_ptr<SessionView> _current_session_view{};

    po::options_description _options_attach { "Allowed options" };
    po::positional_options_description _posiginal_attach {};

    po::options_description _options_findps { "Allowed options" };
    po::positional_options_description _posiginal_findps {};

    po::options_description _options_scan { "Allowed options" };
    po::positional_options_description _posiginal_scan {};

    po::options_description _options_filter { "Allowed options" };
    po::positional_options_description _posiginal_filter {};

    po::options_description _options_update { "Allowed options" };
    po::positional_options_description _posiginal_update {};

public:
    App(TUI& tui, pid_t pid)
        : _tui(tui)
        , _message_view(std::make_shared<MessageView>())
        , _history_view(std::make_shared<HistoryView>())
        , _test_view(std::make_shared<TestView>())
    {
        _options_attach.add_options()("help", "show help message");
        _options_attach.add_options()("pid,p", po::value<pid_t>(), "target process pid");
        _posiginal_attach.add("pid", 1);

        _options_findps.add_options()("help", "show help message");
        _options_findps.add_options()("filter,f", po::value<std::string>(), "regex filter");
        _posiginal_findps.add("filter", 1);

        _options_scan.add_options()("help", "show help message");
        _options_scan.add_options()("step,s", po::bool_switch()->default_value(false), "step size");
        _options_scan.add_options()("I64,q", po::bool_switch()->default_value(false), "64 bit integer");
        _options_scan.add_options()("I32,i", po::bool_switch()->default_value(false), "32 bit integer");
        _options_scan.add_options()("I16,h", po::bool_switch()->default_value(false), "16 bit integer");
        _options_scan.add_options()("I8,b", po::bool_switch()->default_value(false), "8 bit integer");
        _options_scan.add_options()("U64,Q", po::bool_switch()->default_value(false), "64 bit unsigned integer");
        _options_scan.add_options()("U32,I", po::bool_switch()->default_value(false), "32 bit unsigned integer");
        _options_scan.add_options()("U16,H", po::bool_switch()->default_value(false), "16 bit unsigned integer");
        _options_scan.add_options()("U8,B", po::bool_switch()->default_value(false), "8 bit unsigned integer");
        _options_scan.add_options()("expr", po::value<std::string>(), "scan expression");
        _posiginal_scan.add("expr", 1);

        _options_filter.add_options()("help", "show help message");
        _options_filter.add_options()("expr,f", po::value<std::string>(), "filter expression");
        _posiginal_filter.add("expr", 1);

        _options_filter.add_options()("help", "show help message");
        _options_filter.add_options()("session,s", po::value<std::string>(), "filter expression");
        _posiginal_filter.add("session", 1);

        if (pid != -1) {
            _process = std::make_shared<Process>(pid);
        }

        push(_message_view);
    }

    StyleString tui_prompt(size_t width) override
    {
        using namespace ::tui::style;
        StyleStringBuilder builder {};
        builder << SetColor(ColorPrompt) << "Command" << ResetStyle() << "> ";
        return builder.str();
    }

    template <typename T>
    void push(T&& view)
    {
        if (not _view_stack.empty() and _view_stack.top() == view) {
            return;
        }
        _view_stack.push(std::dynamic_pointer_cast<ContentProvider>(view));
        _tui.show(_view_stack.top());
    }

    void pop(bool all = false)
    {
        while (_view_stack.size() > 1) {
            _view_stack.pop();
            if (not all) {
                break;
            }
        }

        _tui.show(_view_stack.top());
        _view_stack.top()->tui_notify_changed();
    }

    void complete() {
        auto& editor = _tui.editor();
        const auto& buffer = editor.buffer();
        
        static std::vector<std::string> keyworks = {
          "help",
          "exit",
          "msg", "mesg", "message",
          "history",
          "back",
          "attach",
          "selfattach",
          "ps", "findps", "findpsex",
          "test",
          "scan",
          "filter",
          "update"
        };

        if (buffer.empty()) {
            return;
        }

        for (auto& str : keyworks) {
            if (str.find(buffer) == 0) {
                _tui.editor().update(str, -1);
                break;
            }
        }
    }

    bool tui_key(int key) override
    {
        switch (key) {
        case KEY_UP:
        case KEY_DOWN:
            _history_view->history_key(key, _tui.editor());
            break;
        case '\t':
            complete();
            return true;
        }
        return false;
    }

    void tui_run(const std::string& line) override
    {
        using namespace tui::style;

        auto [command, arguments] = dsl::parse_command(line);
        if (command.empty()) {
            return;
        }

        if (command == "selfattach") {
            command = "attach";
            arguments = { std::to_string(getpid()) };
        }

        if (command == "exit") {
            _tui.exit();

        } else if (command == "msg" or command == "mesg" or command == "message") {
            push(_message_view);

        } else if (command == "history") {
            push(_history_view);

        } else if (command == "back") {
            pop();

        } else if (command == "attach") {
            po::variables_map vm {};
            PARSE_ARG(attach);

            pid_t pid = -1;

            try {
                if (vm.count("pid")) {
                    pid = vm["pid"].as<pid_t>();
                }
            } catch (const std::exception& e) {
                _message_view->stream()
                    << EnableStyle(AttrUnderline) << SetColor(ColorError) << "Error: " << ResetStyle()
                    << e.what();
                return;
            }

            if (pid == -1) {
                _message_view->stream() << "Usage: attach [options] pid\n"
                                        << _options_attach;
                return;
            }

            _message_view->stream() << "Attach process " << pid;

            pop(true);
            _session_views.clear();
            _process = std::make_shared<Process>(pid);

        } else if (command == "ps") {
            push(std::make_shared<ProcessList>());

        } else if (command == "findpsex" or command == "findps") {
            po::variables_map vm {};
            PARSE_ARG(findps);
            std::string filter {};

            try {
                if (vm.count("filter")) {
                    filter = vm["filter"].as<std::string>();
                }
            } catch (const std::exception& e) {
                _message_view->stream()
                    << EnableStyle(AttrUnderline) << SetColor(ColorError) << "Error: " << ResetStyle()
                    << e.what();
                return;
            }

            if (filter.empty()) {
                _message_view->stream() << "Usage: " << command << " [options] regex\n"
                                        << _options_findps;
                return;
            }

            if (command == "findps") {
                filter = ".*"s + filter + ".*"s;
            }

            push(std::make_shared<ProcessList>(filter));

        } else if (command == "test") {
            push(_test_view);

        } else if (command == "scan") {
            po::variables_map vm {};
            PARSE_ARG(scan);

            ScanConfig config;

            try {
                if (vm.count("expr")) {
                    config._expr = vm["expr"].as<std::string>();
                }
                config._type_bits |= vm["I8"].as<bool>() ? MatchTypeBitI8 : 0;
                config._type_bits |= vm["I16"].as<bool>() ? MatchTypeBitI16 : 0;
                config._type_bits |= vm["I32"].as<bool>() ? MatchTypeBitI32 : 0;
                config._type_bits |= vm["I64"].as<bool>() ? MatchTypeBitI64 : 0;
                config._type_bits |= vm["U8"].as<bool>() ? MatchTypeBitU8 : 0;
                config._type_bits |= vm["U16"].as<bool>() ? MatchTypeBitU16 : 0;
                config._type_bits |= vm["U32"].as<bool>() ? MatchTypeBitU32 : 0;
                config._type_bits |= vm["U64"].as<bool>() ? MatchTypeBitU64 : 0;
            } catch (const std::exception& e) {
                _message_view->stream()
                    << EnableStyle(AttrUnderline) << SetColor(ColorError) << "Error: " << ResetStyle()
                    << e.what();
                return;
            }

            if (config._expr.empty()) {
                _message_view->stream() << "Usage: " << command << " [options] expression\n"
                                        << _options_scan;
                return;
            }

            if (not _process) {
                _message_view->stream() << "Invalid target process, attach to target using the 'attach' command";
                return;
            }

            try {
                auto view = scan(_message_view, _process, config);
                if (view) {
                    _session_views.emplace_back(view);
                    _current_session_view = view;
                    push(view);
                }
            } catch(const std::exception& e) {
                _message_view->stream()
                    << EnableStyle(AttrUnderline) << SetColor(ColorError) << "Error:" << ResetStyle() << " "
                    << e.what();
                return;
            }

        } else if (command == "filter") {
            po::variables_map vm {};
            PARSE_ARG(filter);

            ScanConfig config;

            try {
                if (vm.count("expr")) {
                    config._expr = vm["expr"].as<std::string>();
                }
            } catch (const std::exception& e) {
                _message_view->stream()
                    << EnableStyle(AttrUnderline) << SetColor(ColorError) << "Error: " << ResetStyle()
                    << e.what();
                return;
            }

            if (config._expr.empty()) {
                _message_view->stream() << "Usage: " << command << " [options] expression\n"
                                        << _options_filter;
                return;
            }

            if (_current_session_view == nullptr) {
                _message_view->stream()
                    << SetColor(ColorError)
                    << "Error:"
                    << ResetStyle()
                    << " No avaliable session, create a session using the 'scan' command";
                return;
            }

            if (not _process) {
                _message_view->stream() << "Invalid target process: attach to target using the 'attach' command";
                return;
            }

            try {
                filter(_message_view, _current_session_view, config);
            } catch(const std::exception& e) {
                _message_view->stream()
                    << EnableStyle(AttrUnderline) << SetColor(ColorError) << "Error:" << ResetStyle() << " "
                    << e.what();
                return;
            }

        } else if (command == "update") {
            if (_current_session_view == nullptr) {
                _message_view->stream()
                    << SetColor(ColorError)
                    << "Error:"
                    << ResetStyle()
                    << " No avaliable session, create a session using the 'scan' command";
                return;
            }

            update(_message_view, _current_session_view);

        } else {
            using namespace tui::style;
            _message_view->stream()
                << EnableStyle(AttrUnderline) << SetColor(ColorWarning) << "Unknown command:"
                << ResetStyle() << " " << line;
            push(_message_view);
        }

        _history_view->push_back(command);
    }
};

int main(int argc, char* argv[])
{
    pid_t target_pid = -1;

    try {
        po::options_description desc("Allowed options");
        desc.add_options()("help", "show help message")("pid,p", po::value<pid_t>(), "target process pid");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << desc << std::endl;
            return 0;
        }

        if (vm.count("pid")) {
            target_pid = vm["pid"].as<pid_t>();
        }
    } catch (std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }

    TUI tui { TUIFlagColor };
    tui.attach(std::make_shared<App>(tui, target_pid));
    tui.run();
    return 0;
}
