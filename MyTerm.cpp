#include <X11/Xlib.h>//Core X11 functions for creating and managing windows, drawing text, handling events.
#include <X11/Xutil.h>//Utility functions for X11 (fonts, window properties, etc.).
#include <X11/keysym.h>//X11 keysyms (keyboard symbols)
#include <X11/Xatom.h>//X11 atoms (for window properties, etc.)
#include <iostream> //For standard I/O operations
#include <string>   //For using std::string
#include <vector>   //For using std::vector
#include <unistd.h> //For POSIX operating system API
#include <sys/wait.h> //For process control (fork, exec, wait)
#include <fcntl.h>  //For file control options (open, fcntl)
#include <sys/select.h>  //For multiplexing I/O (select function)
#include <sstream> //For stringstream
#include <ctype.h> //For character classification and conversion
#include <cerrno>  //For error number macros
#include <cstring> //For strerror()
#include <locale.h>//For setting locale
#include <stdlib.h> // For getenv()
#include <glob.h>   //For filename pattern matching (globbing)
#include <signal.h> //For signal handling (SIGINT, SIGTSTP, SIGCHLD)
#include <ctime>   //For timestamping in multiWatch
#include <algorithm>//For functions like sort(), find(), etc.

using namespace std;

//Flags for signal handlers
volatile bool g_interrupt_flag = false;// Set to true when Ctrl+C (SIGINT) is received to interrupt running command
volatile bool g_sigchld_received = false;// Set to true when a child process terminates (SIGCHLD received)

void interrupt_handler(int sig) {
    g_interrupt_flag = true;// Signal handler for Ctrl+C (SIGINT). Sets flag to tell shell to interrupt current running command
}

// SIGCHLD handler for robust zombie reaping
void sigchld_handler(int sig) {
    g_sigchld_received = true;// Signal handler for SIGCHLD. Sets flag to indicate a child process has terminated so parent can reap it
}

// ShellTab struct to hold state for each tab
// --- Q1: GUI - Each tab holds an independent shell instance ---
struct ShellTab {
    vector<string> textBuffer;// Stores all lines displayed in the terminal for this tab
    string currentCommand;// Q1: GUI Buffer/Input
    int cursorPosition; // Q8: Line Navigation
    string multiLineCommand;// Q3: Multiline Input
    vector<pid_t> childPids;// PIDs of child processes spawned from this tab
    int outputPipeFd;
    int scrollOffset;
    vector<string> history; // Q10: History
    int historyIndex;      // Q10: History           
    vector<pair<pid_t, string>> jobs;// Background jobs: pair of PID and command string
    pid_t foreground_pgid;
    string currentForegroundCommand;// Command currently running in foreground
    bool isForeground;
    bool isMultiWatchRunning; 
    bool isSearching;
    string searchTerm;
    string searchResult;
    int searchHistoryIndex;
    bool isChoosingCompletion;
    vector<string> completionChoices;// List of possible file completions for auto-complete

     // Constructor: initializes default values for a new tab
    // --- Q1: GUI ---
    ShellTab() : cursorPosition(0), outputPipeFd(-1), scrollOffset(0), historyIndex(-1),
                 isForeground(false), foreground_pgid(0), isMultiWatchRunning(false), 
                 isSearching(false), searchHistoryIndex(-1), isChoosingCompletion(false) {
        textBuffer.push_back("Welcome to a new MyTerm tab!");
    }
};

// Function Prototypes

// Redraws the entire terminal window for a given tab
void redraw(Display* display, Window window, GC gc, XFontSet fontSet, vector<ShellTab>& tabs, int activeTabIndex);

// Executes the command currently typed in the active tab
void executeCommand(Display* display, Window window, GC gc, XFontSet fontSet, vector<ShellTab>& tabs, int activeTabIndex);

// Performs history search (Ctrl+R) in the given tab
void performSearch(ShellTab& tab);

// Finalizes search results and updates the command line
bool finalizeSearch(ShellTab& tab);

// Removes leading and trailing whitespace characters from a string
string trim(const string& str) {
    const string WHITESPACE = " \n\r\t\f\v";
    size_t first = str.find_first_not_of(WHITESPACE);
    if (string::npos == first) return ""; // Empty string if all whitespace
    size_t last = str.find_last_not_of(WHITESPACE);
    return str.substr(first, (last - first + 1));
}

// Processes escape sequences like \n, \t, \\, \" in a string
// --- Q3: Multiline/Unicode Input (Handles escapes) ---
string processEscapes(const string& str) {
    string result;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '\\' && i + 1 < str.length()) {
            switch (str[i + 1]) {
                case 'n': result += '\n'; i++; break;
                case 't': result += '\t'; i++; break;
                case '\\': result += '\\'; i++; break;
                case '"': result += '"'; i++; break;
                default: result += str[i];// Keep the backslash if unknown escape
            }
        } else {
            result += str[i];
        }
    }
    return result;
}

// Splits a command string into arguments (handles quotes and escape sequences)
// --- Q2/Q3: Parse command for execution, respects quotes for Q3 -
vector<string> parseCommand(const string& command) {
    vector<string> args;
    string currentArg;
    bool inQuotes = false;// Track if inside double quotes
    for (char c : command) {
        if (c == ' ' && !inQuotes) { // Space separates arguments outside quotes
            if (!currentArg.empty()) {
                args.push_back(processEscapes(currentArg));
                currentArg.clear();
            }
        } else if (c == '"') {  // Toggle quote mode
            inQuotes = !inQuotes;
        } else {
            currentArg += c;
        }
    }
    if (!currentArg.empty()) {
        args.push_back(processEscapes(currentArg));
    }
    return args;
}

// redraw function (modified to handle scrolling and command line rendering)
// --- Q1: GUI - Main drawing function ---
void redraw(Display* display, Window window, GC gc, XFontSet fontSet, vector<ShellTab>& tabs, int activeTabIndex) {
    XClearWindow(display, window);// Clear the entire window before redrawing
    int screen = DefaultScreen(display);

    //Draw Tabs
    // --- Q1: GUI - Draw Tabs ---
    int tabWidth = 100, tabHeight = 30;
    for (size_t i = 0; i < tabs.size(); ++i) {
        int x = i * tabWidth;
        if ((int)i == activeTabIndex) {
            // Active tab: white background, black text
            XSetForeground(display, gc, WhitePixel(display, screen));
            XFillRectangle(display, window, gc, x, 0, tabWidth, tabHeight);
            XSetForeground(display, gc, BlackPixel(display, screen));
        } else {
            // Inactive tab: black background, white text
            XSetForeground(display, gc, BlackPixel(display, screen));
            XFillRectangle(display, window, gc, x, 0, tabWidth, tabHeight);
            XSetForeground(display, gc, WhitePixel(display, screen));
        }

        // Draw tab label ("Tab 1", "Tab 2", ...)
        string tabLabel = "Tab " + to_string(i + 1);
        Xutf8DrawString(display, window, fontSet, gc, x + 10, 20, tabLabel.c_str(), tabLabel.length());

        // Draw tab border
        XSetForeground(display, gc, BlackPixel(display, screen));
        XDrawRectangle(display, window, gc, x, 0, tabWidth, tabHeight);
    }
    
    //Draw '+' button to create new tab
    // --- Q1: GUI - Draw New Tab Button ---
    int plusButtonX = tabs.size() * tabWidth;
    XSetForeground(display, gc, BlackPixel(display, screen));
    XFillRectangle(display, window, gc, plusButtonX, 0, 30, tabHeight);
    XSetForeground(display, gc, WhitePixel(display, screen));
    Xutf8DrawString(display, window, fontSet, gc, plusButtonX + 11, 20, "+", 1);
    XSetForeground(display, gc, BlackPixel(display, screen));
    XDrawRectangle(display, window, gc, plusButtonX, 0, 30, tabHeight);


    // If no valid active tab, exit
    if (activeTabIndex < 0 || (size_t)activeTabIndex >= tabs.size()) return;
    
    ShellTab& activeTab = tabs[activeTabIndex];

    // --- Calculate text display area ---
    // --- Q1: GUI - Draw Output Buffer ---
    int y_offset = 55, line_height = 25;
    // Start drawing text below tabs
    // Height of each line
    int window_height = 600;// Total window height (assumed)
    int visible_lines = (window_height - y_offset) / line_height;
    

    // Adjust scrollOffset if out of bounds
    if (activeTab.scrollOffset > (int)activeTab.textBuffer.size() - visible_lines + 1) {
        activeTab.scrollOffset = max(0, (int)activeTab.textBuffer.size() - visible_lines + 1);
    }

    int start_line = activeTab.scrollOffset;
    int end_line = min((int)activeTab.textBuffer.size(), start_line + visible_lines);

    // Draw each line of text within the visible range
    int current_y = y_offset;
    for (int i = start_line; i < end_line; ++i) {
        Xutf8DrawString(display, window, fontSet, gc, 10, current_y, activeTab.textBuffer[i].c_str(), activeTab.textBuffer[i].length());
        current_y += line_height;
    }
    
    // Draw command line at the bottom/ search / completion prompt
    if (activeTab.scrollOffset >= (int)activeTab.textBuffer.size() - visible_lines + 1) {
        string full_line;

        if (activeTab.isSearching) {
            // If history search active (Ctrl+R)
            string prompt = "(reverse-i-search)" + activeTab.searchTerm + ": ";
            full_line = prompt + activeTab.searchResult;
            Xutf8DrawString(display, window, fontSet, gc, 10, current_y, full_line.c_str(), full_line.length());
        } 
        else if (activeTab.isChoosingCompletion) {
            // If user is selecting a file completion
            // --- Q11: GUI for Auto-complete ---
            string prompt = "Choose a completion: ";
            Xutf8DrawString(display, window, fontSet, gc, 10, current_y, prompt.c_str(), prompt.length());
        } else  // Normal command line prompt
            {
                // --- Q1: GUI - Draw Prompt ---
            // --- Q3: Handle multiline prompt ('> ') ---
            string prompt = activeTab.multiLineCommand.empty() ? "user@myterm> " : "> ";
            full_line = prompt + activeTab.currentCommand;
            Xutf8DrawString(display, window, fontSet, gc, 10, current_y, full_line.c_str(), full_line.length());

            //Draw cursor
            // --- Q1/Q8: Draw Cursor based on cursorPosition ---
            string before_cursor = prompt + activeTab.currentCommand.substr(0, activeTab.cursorPosition);
            XRectangle ink, logical;
            Xutf8TextExtents(fontSet, before_cursor.c_str(), before_cursor.length(), &ink, &logical);
            int cursor_x = 10 + logical.width;
            XDrawLine(display, window, gc, cursor_x, current_y - line_height + 5, cursor_x, current_y + 2);
        }
    }
    
    XFlush(display);// Flush X11 commands to update the window immediately
}

// Signal handler for SIGINT during multiWatch
// parseMultiWatch function to extract commands from multiwatch
// --- Q7: multiWatch - Command Parser ---
vector<string> parseMultiWatch(const string& finalCommand) {
    vector<string> watch_cmds;

    // Find the opening '['
    size_t lbr = finalCommand.find('[');
    if (lbr == string::npos) return watch_cmds;// No '[' found → empty vector

    // Find the closing ']' (or use end of string if not found)
    size_t rbr = finalCommand.find(']', lbr + 1);
    if (rbr == string::npos) rbr = finalCommand.length();

    // Extract the substring inside the brackets
    string inside = finalCommand.substr(lbr + 1, rbr - lbr - 1);
    string cur;             // Current command being parsed
    bool in_dq = false, in_sq = false;      // Inside double quotes  //// Inside single quotes

    // Iterate character by character
    for (size_t i = 0; i < inside.size(); ++i) {
        char c = inside[i];
        if (c == '"' && !in_sq) { in_dq = !in_dq; cur += c; }// Toggle double-quote mode 
        else if (c == '\'' && !in_dq) { in_sq = !in_sq; cur += c; }   // Toggle single-quote mode
        else if (c == ',' && !in_dq && !in_sq) {// Comma outside quotes → end of a command
            string token = trim(cur);
            if (!token.empty()) {// Remove surrounding quotes if any
                if ((token.front() == '"' && token.back() == '"') || (token.front() == '\'' && token.back() == '\'')) {
                    token = token.substr(1, token.length() - 2);
                }
                watch_cmds.push_back(processEscapes(token));// Process escape sequences
            }
            cur.clear();
        } else { cur += c; } // Accumulate character in current command
    }

    // Add the last command after the loop
    string token = trim(cur);
    if (!token.empty()) {// Remove surrounding quotes if any
        if ((token.front() == '"' && token.back() == '"') || (token.front() == '\'' && token.back() == '\'')) {
            token = token.substr(1, token.length() - 2);
        }
        watch_cmds.push_back(processEscapes(token));
    }
    return watch_cmds;// Return vector of parsed commands
}


//executeCommand Function
void executeCommand(Display* display, Window window, GC gc, XFontSet fontSet, vector<ShellTab>& tabs, int activeTabIndex) {
    ShellTab& activeTab = tabs[activeTabIndex];
    string finalCommand = activeTab.multiLineCommand + activeTab.currentCommand;
    
    //// --- Add to command history (up to 10,000 commands) ---
    if (!finalCommand.empty()) {
        if (activeTab.history.empty() || activeTab.history.back() != finalCommand) {
            activeTab.history.push_back(finalCommand);
             if(activeTab.history.size() > 10000) {
                activeTab.history.erase(activeTab.history.begin());
            }
        }
        activeTab.historyIndex = activeTab.history.size();
    }

    // Display the command in the text buffer
     // --- Q1: Display executed command in GUI ---
    // --- Q3: Handle multiline prompt display ---
    activeTab.textBuffer.push_back((activeTab.multiLineCommand.empty() ? "user@myterm> " : "> ") + finalCommand);
    activeTab.multiLineCommand.clear();

    //Split commands by pipe '|' for pipeline execution
    // --- Q6: Split command by pipes ---
    vector<string> commands;
    stringstream ss(finalCommand);
    string segment;
    while(getline(ss, segment, '|')) {
        commands.push_back(trim(segment));
    }

    if (commands.empty() || trim(finalCommand).empty()) return;

    //multiWatch
    // --- Q7: multiWatch Implementation ---
    string trimmed_command = trim(commands[0]);
    string lower_trim = trimmed_command;
    transform(lower_trim.begin(), lower_trim.end(), lower_trim.begin(), ::tolower);
    if (lower_trim.rfind("multiwatch", 0) == 0) {
        activeTab.isMultiWatchRunning = true; 
        vector<string> watch_cmds = parseMultiWatch(finalCommand);
        if (watch_cmds.empty()) {
            activeTab.textBuffer.push_back("multiwatch: Invalid format or no commands provided.");
            activeTab.isMultiWatchRunning = false;
            return;
        }

        //Setup child processes and pipes for multiWatch 
        vector<pid_t> pids;
        vector<int> read_fds;
        g_interrupt_flag = false;
        signal(SIGINT, interrupt_handler); 
        activeTab.textBuffer.push_back("multiwatch: starting " + to_string(watch_cmds.size()) + " commands. Press Ctrl+C to stop.");
        

        // --- Q7: Fork processes for each command (parallel execution) ---
        for (const auto& cmd : watch_cmds) {
            int ppipe[2];
            if (pipe(ppipe) == -1) { perror("pipe"); continue; }
            pid_t pid = fork();
            if (pid == -1) { perror("fork"); close(ppipe[0]); close(ppipe[1]); continue; }// Fork failed
            if (pid == 0) { // child
                signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
                close(ppipe[0]);
                dup2(ppipe[1], STDOUT_FILENO); dup2(ppipe[1], STDERR_FILENO);
                close(ppipe[1]);
                if (cmd.empty()) _exit(EXIT_SUCCESS);
                execlp("sh", "sh", "-c", cmd.c_str(), (char*)NULL);
                perror("execlp(sh -c)");
                _exit(EXIT_FAILURE);
            } else { // parent
                close(ppipe[1]); pids.push_back(pid); read_fds.push_back(ppipe[0]);
            }
        }

        // Monitor pipes and X11 events
        //Read output from multiple children in non-blocking way using select() 
        // --- Q7: Monitor outputs using select() ---
        int remaining = pids.size();
        while (remaining > 0 && !g_interrupt_flag) {// Q7: Stop on Ctrl+C flag
            fd_set rfds;
            FD_ZERO(&rfds);// Initialize read fd set
            int max_fd = -1;
            for (int fd : read_fds) if (fd != -1) { FD_SET(fd, &rfds); if (fd > max_fd) max_fd = fd; }
            
            FD_SET(ConnectionNumber(display), &rfds);// Q1: Monitor GUI events
            if (ConnectionNumber(display) > max_fd) max_fd = ConnectionNumber(display);

            if (max_fd == -1) break; // All pipes closed

            timeval tv{1, 0}; // 1 second timeout
            int sel_ret = select(max_fd + 1, &rfds, nullptr, nullptr, &tv);// Wait for data or GUI event

            if (sel_ret < 0) { if (errno == EINTR) continue; perror("select"); break; }// Select error

            //Handle GUI events while multiWatch runs
            // --- Q1: Handle GUI events during multiWatch ---
            if (FD_ISSET(ConnectionNumber(display), &rfds)) {// GUI event available
                while(XPending(display)) {//    Process all pending events
                    XEvent ev;
                    XNextEvent(display, &ev);// Get next event
                    if (ev.type == Expose) {
                         redraw(display, window, gc, fontSet, tabs, activeTabIndex);// Redraw on expose
                    } else if (ev.type == ClientMessage) {
                        if ((Atom)ev.xclient.data.l[0] == XInternAtom(display, "WM_DELETE_WINDOW", False)) {// Handle window close
                            g_interrupt_flag = true; 
                        }
                    }
                }
            }
            
            //Read from child pipes
            // --- Q7: Read output and format with timestamp ---
            for (size_t i = 0; i < read_fds.size(); ++i) {
                int fd = read_fds[i];
                if (fd != -1 && FD_ISSET(fd, &rfds)) {// Data available to read
                    char buf[4096];
                    ssize_t n = read(fd, buf, sizeof(buf) - 1);// Leave space for null terminator
                    if (n > 0) {
                        buf[n] = '\0';//// Print output with timestamp
                        time_t now = time(0);
                        char timestamp[100];
                        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
                        string header = "\"" + (i < watch_cmds.size() ? watch_cmds[i] : string("<cmd>")) + "\", " + timestamp + ":";
                        activeTab.textBuffer.push_back(header);
                        activeTab.textBuffer.push_back("----------------------------------------------------");
                        stringstream ss(buf);// Split output into lines
                        string line;
                        while (getline(ss, line)) if (!line.empty()) activeTab.textBuffer.push_back(line);
                        activeTab.textBuffer.push_back("----------------------------------------------------");
                        redraw(display, window, gc, fontSet, tabs, activeTabIndex);
                    } else if (n == 0 || (n < 0 && errno != EAGAIN)) {// Pipe closed
                        close(fd);// EOF or error
                        waitpid(pids[i], NULL, 0);// Reap child
                        read_fds[i] = -1;
                        remaining--;
                    }
                }
            }
        }

        //Cleanup after multiWatch ends
        // --- Q7: Cleanup multiWatch children ---
        signal(SIGINT, SIG_IGN); 
        for (int fd : read_fds) if (fd != -1) close(fd);// Close all pipes
        for (pid_t pid : pids) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
        }
        activeTab.textBuffer.push_back("multiwatch terminated.");// Indicate multiWatch ended
        activeTab.isMultiWatchRunning = false; 
        return;
    }
    //  END of multiWatch logic

    //Checked for built-ins *before* checking for '&'
    vector<string> args = parseCommand(commands[0]);// Parse first command for built-in check
    if (!args.empty()) {
        if (args[0] == "cd") {//Built-in implementations (cd, clear, pwd, history, jobs, fg, bg)
            if (args.size() < 2) {// No argument → go to HOME
                const char* homeDir = getenv("HOME");
                if (homeDir && chdir(homeDir) != 0) activeTab.textBuffer.push_back("cd: " + string(strerror(errno)));// Change to HOME
            } else {// Change to specified directory
                string path = args[1];
                if (!path.empty() && path[0] == '~') {
                    const char* homeDir = getenv("HOME");
                    if (homeDir) path.replace(0, 1, homeDir);// Expand '~' to HOME
                }
                if (chdir(path.c_str()) != 0) activeTab.textBuffer.push_back("cd: " + string(strerror(errno)));// Change directory
            }
            return; // Built-in finished
        } else if (args[0] == "clear") {
            // --- Q10: 'history' command (Spec 10b) ---
            activeTab.textBuffer.clear();
            return; // Built-in finished
        } else if (args[0] == "pwd") {// --- Q9: 'jobs' command ---
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                activeTab.textBuffer.push_back(cwd);
            } else {
                activeTab.textBuffer.push_back("pwd: " + string(strerror(errno)));
            }
            return; // Built-in finished
        } else if (args[0] == "history") {
            int start = max(0, (int)activeTab.history.size() - 1000);// Show last 1000 entries
            for(size_t i = start; i < activeTab.history.size(); ++i) {//    Display history with numbering
                string entry = "  " + to_string(i + 1) + "  " + activeTab.history[i];// Format history entry
                activeTab.textBuffer.push_back(entry);
            }
            return; // Built-in finished
        } else if (args[0] == "jobs") {
            for (size_t i = 0; i < activeTab.jobs.size(); ++i) {// List background jobs
                pid_t pgid = activeTab.jobs[i].first;
                string cmd = activeTab.jobs[i].second;
                string entry = "[" + to_string(i+1) + "] " + to_string(pgid) + " " + cmd;
                
                // Check status without blocking
                int status;
                pid_t result = waitpid(-pgid, &status, WNOHANG | WUNTRACED);
                if (result == -1 && errno == ECHILD) {
                    entry += " (Done)"; // Job group no longer exists
                } else if (result > 0) {
                    if (WIFSTOPPED(status)) entry += " (Stopped)";
                    else if (WIFEXITED(status) || WIFSIGNALED(status)) entry += " (Done)";
                    else entry += " (Running)";
                } else {
                    entry += " (Running)";
                }
                activeTab.textBuffer.push_back(entry);
            }
            // Clean up jobs marked (Done) - simpler to do in SIGCHLD handler
            return; // Built-in finished
        } 
        else if (args[0] == "fg") {//Bring job to foreground
            // --- Q9: 'fg' command ---
            if (activeTab.isForeground) {
                activeTab.textBuffer.push_back("fg: a job is already in the foreground."); return;
            }
            if (args.size() < 2) { activeTab.textBuffer.push_back("fg: usage: fg <jobnum>"); return; }
            int jobnum = atoi(args[1].c_str()) - 1;
            if (jobnum < 0 || (size_t)jobnum >= activeTab.jobs.size()) { activeTab.textBuffer.push_back("fg: no such job"); return; }

            pid_t pgid = activeTab.jobs[jobnum].first;
            string cmd = activeTab.jobs[jobnum].second;
            activeTab.jobs.erase(activeTab.jobs.begin() + jobnum);

            if (kill(-pgid, SIGCONT) < 0) {// Q9: Send SIGCONT to the job's process group
                perror("fg: kill (SIGCONT)");
                return;
            }

            activeTab.isForeground = true;// Mark as foreground job
            activeTab.foreground_pgid = pgid;// Track foreground pgid
            activeTab.currentForegroundCommand = cmd;// Track current foreground command
            
            int status;
            if (waitpid(-pgid, &status, WUNTRACED) < 0) {// Wait for the foreground job
                 perror("fg: waitpid");
            }
            
            activeTab.isForeground = false;// Foreground job finished
            activeTab.foreground_pgid = 0;// Clear foreground pgid
            activeTab.currentForegroundCommand.clear();// Clear current foreground command

            if (WIFSTOPPED(status)) {// Job was stopped// Q9
                activeTab.jobs.emplace_back(pgid, cmd);
                activeTab.textBuffer.push_back("[" + to_string(activeTab.jobs.size()) + "]+  Stopped  " + cmd);
            }
            return; // Built-in finished
        } 
        else if (args[0] == "bg") {//Resume job in background
            // --- Q9: 'bg' command ---
            if (args.size() < 2) { activeTab.textBuffer.push_back("bg: usage: bg <jobnum>"); return; }// No job number provided
            int jobnum = atoi(args[1].c_str()) - 1;// Convert to 0-based index
            if (jobnum < 0 || (size_t)jobnum >= activeTab.jobs.size()) { activeTab.textBuffer.push_back("bg: no such job"); return; }// Invalid job number
            
            pid_t pgid = activeTab.jobs[jobnum].first;// Get job's pgid
            
            if (kill(-pgid, SIGCONT) < 0) {// Send SIGCONT to the job's process group
                perror("bg: kill (SIGCONT)");
            } else {// Success
                 activeTab.textBuffer.push_back("[" + to_string(jobnum+1) + "] " + to_string(pgid) + " continued");
            }
            return; // Built-in finished
        } 
    }

    // --- Background job support (now after built-in check) ---
    // --- Q9: Background job support (&) ---
    bool runInBackground = false;
    if (!commands.empty()) { // Need to re-check, commands[0] might be empty
        string& first_cmd = commands[0];
        if (!first_cmd.empty()) {// Check if last character is '&'
            size_t pos = first_cmd.find_last_not_of(" \t");// Find last non-whitespace
            if (pos != string::npos && first_cmd[pos] == '&') {// '&' found
                runInBackground = true;
                first_cmd = first_cmd.substr(0, pos);
                first_cmd = trim(first_cmd);
            }
        }
    }
    
    // EXTERNAL COMMAND / PIPELINE EXECUTION 
    // --- Q2/Q6: EXTERNAL COMMAND / PIPELINE EXECUTION ---
    int num_commands = commands.size();
    int input_fd = STDIN_FILENO;
    int pipefds[2];
    activeTab.childPids.clear();
    activeTab.currentForegroundCommand.clear(); 
    pid_t pgid = 0;

    for (int i = 0; i < num_commands; ++i) {
        if (pipe(pipefds) == -1) {// Q6: Create pipe
            perror("pipe");
            return;
        }

        // --- Refinement: Check for empty command in pipe ---
        if (trim(commands[i]).empty()) {
            if (i == 0) activeTab.textBuffer.push_back("Syntax error: empty command at start of pipe.");
            else activeTab.textBuffer.push_back("Syntax error: empty command in pipe.");
            
            if (input_fd != STDIN_FILENO) close(input_fd);
            close(pipefds[0]);
            close(pipefds[1]);
            return;
        }

        pid_t pid = fork();// Q2/Q6: Fork for pipeline segment
        if (pid == -1) {
            perror("fork");
            return;
        }

        if (pid == 0) { // --- CHILD PROCESS ---
            signal(SIGINT, SIG_DFL); // Q9: Child processes respect Ctrl+C
            signal(SIGTSTP, SIG_DFL);// Q9: Child processes respect Ctrl+Z
            signal(SIGCHLD, SIG_DFL); // --- NEW: Child shouldn't catch SIGCHLD

            if (pgid == 0) pgid = getpid();
            setpgid(0, pgid);// Q9: Set Process Group ID for job control

            if (input_fd != STDIN_FILENO) {
                dup2(input_fd, STDIN_FILENO);// Q6: Connect pipe input
                close(input_fd);
            }
            
            // Output redirection
            if (i < num_commands - 1) { // Not last command
                dup2(pipefds[1], STDOUT_FILENO); // Q6: Connect pipe output
                dup2(pipefds[1], STDERR_FILENO);
            } else { // Last command
                 dup2(pipefds[1], STDOUT_FILENO); // Q1: Connect output to parent's reader
                 dup2(pipefds[1], STDERR_FILENO);
            }
            
            close(pipefds[0]);// Close unused read end
            close(pipefds[1]);// Close original write end
            
            vector<string> child_args = parseCommand(commands[i]);
            string inputFile, outputFile;
            bool appendMode = false;
            vector<string> commandArgs;
             // --- Q4/Q5: Parse I/O Redirection ---
            for(size_t j = 0; j < child_args.size(); ++j) {
                if (child_args[j] == "<") { if(j+1 < child_args.size()) inputFile = child_args[++j]; }
                else if (child_args[j] == ">") { if(j+1 < child_args.size()) outputFile = child_args[++j]; appendMode = false; }
                else if (child_args[j] == ">>") { if(j+1 < child_args.size()) outputFile = child_args[++j]; appendMode = true; }
                else { commandArgs.push_back(child_args[j]); }
            }
            if (!inputFile.empty()) {// Input redirection
                 // --- Q4 / Q5a: Handle file input redirection ---
                int fd_in = open(inputFile.c_str(), O_RDONLY);
                if (fd_in != -1) { dup2(fd_in, STDIN_FILENO); close(fd_in); } else { perror("open input"); exit(EXIT_FAILURE); }
            }
            if (!outputFile.empty()) {// Output redirection
                // --- Q5 / Q5a: Handle file output redirection ---
                int flags = O_WRONLY | O_CREAT | (appendMode ? O_APPEND : O_TRUNC);
                int fd_out = open(outputFile.c_str(), flags, 0644);
                if (fd_out != -1) { 
                    dup2(fd_out, STDOUT_FILENO); 
                    dup2(fd_out, STDERR_FILENO); 
                    close(fd_out); 
                } else { perror("open output"); exit(EXIT_FAILURE); }
            }
            vector<string> expanded_args;
            if (!commandArgs.empty()) {// Globbing for each argument except the command itself
                expanded_args.push_back(commandArgs[0]);
                for (size_t j = 1; j < commandArgs.size(); ++j) {
                    glob_t glob_result;
                    // This is file globbing (e.g., ls *.txt), not Q11 auto-complete
                    if (glob(commandArgs[j].c_str(), GLOB_TILDE | GLOB_NOCHECK, NULL, &glob_result) == 0) {
                        for(size_t k = 0; k < glob_result.gl_pathc; ++k) {
                            expanded_args.push_back(string(glob_result.gl_pathv[k]));
                        }
                    }
                    globfree(&glob_result);// Free glob result
                }
            }
            if (expanded_args.empty()) { exit(EXIT_SUCCESS); }
            vector<char*> argv;
            for(const auto& arg : expanded_args) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());// Q2: Execute external command
            perror("execvp");
            exit(EXIT_FAILURE);

        } else { // --- PARENT PROCESS ---
            if (pgid == 0) pgid = pid;// Set pgid to first child's PID
            setpgid(pid, pgid);// Q9: Parent sets child PGID

            if (input_fd != STDIN_FILENO) { // Close previous read end
                close(input_fd);
            }
            
            close(pipefds[1]); 
            input_fd = pipefds[0]; // Q6: Save read end for next command or parent
            
            activeTab.childPids.push_back(pid);// Save read end of last pipe // Q1/Q2/Q6
        }
    }

    activeTab.outputPipeFd = input_fd;// Save read end of last pipe
    
    if (runInBackground) {// Background job
        // --- Q9: Handle background job ---
        if (pgid != 0) {// Add to jobs list
            activeTab.jobs.emplace_back(pgid, finalCommand); 
            activeTab.textBuffer.push_back("[" + to_string(activeTab.jobs.size()) + "] " + to_string(pgid));
        }
        if (activeTab.outputPipeFd != STDIN_FILENO && activeTab.outputPipeFd != -1) {
            close(activeTab.outputPipeFd); 
        }
        activeTab.outputPipeFd = -1;
    } else if (activeTab.outputPipeFd != -1) { // --- Q9: Handle foreground job ---
        fcntl(activeTab.outputPipeFd, F_SETFL, O_NONBLOCK);// Set pipe to non-blocking
        activeTab.foreground_pgid = pgid;
        activeTab.isForeground = true;
        activeTab.currentForegroundCommand = finalCommand;
    }
}

// (Helper functions: get_word_before_cursor, find_longest_common_prefix, performSearch)
// --- Q11: Auto-complete helper ---
string get_word_before_cursor(const string& text, int cursor) {
    if (cursor == 0) return "";// No word before cursor
    size_t end = cursor;
    size_t start = text.rfind(' ', end - 1);// Find last space before cursor
    if (start == string::npos) { start = 0; } else { start += 1; }// Move past the space
    return text.substr(start, end - start);
}

// Finds the longest common prefix among a vector of strings
// --- Q11: Auto-complete helper (Spec 11.ii) ---
string find_longest_common_prefix(const vector<string>& strs) {// Find longest common prefix among strings
    if (strs.empty()) return "";// No strings → empty prefix
    string prefix = strs[0];
    for (size_t i = 1; i < strs.size(); ++i) {
        while (strs[i].find(prefix) != 0) {// Not a prefix
            prefix = prefix.substr(0, prefix.length() - 1);
            if (prefix.empty()) return "";// No common prefix
        }
    }
    return prefix;
}

// --- Q10: History Search (Ctrl+R) logic ---
void performSearch(ShellTab& tab) {// Searches through command history for the given search term (used in Ctrl+R reverse search)
    if (tab.searchTerm.empty()) {
        tab.searchResult = "";// No search term → empty result
        return;
    }
    int start_index = (tab.searchHistoryIndex != -1) ? tab.searchHistoryIndex : tab.history.size() - 1;
    for (int i = start_index; i >= 0; --i) {
        if (tab.history[i].find(tab.searchTerm) != string::npos) {
            tab.searchResult = tab.history[i];
            tab.searchHistoryIndex = i - 1;
            return;
        }
    }
    if (tab.searchHistoryIndex != -1) {// Wrap around search
         for (int i = tab.history.size() - 1; i > start_index; --i) {
             if (tab.history[i].find(tab.searchTerm) != string::npos) {
                tab.searchResult = tab.history[i];
                tab.searchHistoryIndex = i - 1;
                return;
            }
         }
    }
    tab.searchResult = "";// No match found
    tab.searchHistoryIndex = -1;// Reset search index
}

// (Helper: longestCommonSubstringLength)
// --- Q10: History Search (Substring match) helper (Spec 10d) ---
int longestCommonSubstringLength(const string& s1, const string& s2) {
    if (s1.empty() || s2.empty()) return 0;
    vector<vector<int>> dp(s1.length() + 1, vector<int>(s2.length() + 1, 0));
    int maxLength = 0;
    for (size_t i = 1; i <= s1.length(); ++i) {
        for (size_t j = 1; j <= s2.length(); ++j) {
            if (s1[i - 1] == s2[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
                if (dp[i][j] > maxLength) {
                    maxLength = dp[i][j];
                }
            } else {
                dp[i][j] = 0;
            }
        }
    }
    return maxLength;
}

//finalizeSearch function
// --- Q10: History Search (Finalize) logic ---
bool finalizeSearch(ShellTab& tab) {
    // --- FIX: If i-search already found a result, accept it ---
    if (!tab.searchResult.empty()) {
        return true;
    }

    // If no i-search result, search as per spec [cite: 133, 134]
    if (tab.searchTerm.empty()) {
        tab.searchResult = "";
        return false;
    }

   
     // 1. Search for most recent exact match 
    // --- Q10: (Spec 10c) Exact match ---
    for (int i = tab.history.size() - 1; i >= 0; --i) {
        if (tab.history[i] == tab.searchTerm) {
            tab.searchResult = tab.history[i];
            return true;
        }
    }

    // 2. If no exact match, find most recent longest substring match > 2 
    int maxLen = 2; 
    string bestMatch = "";
    for (int i = tab.history.size() - 1; i >= 0; --i) {
        int len = longestCommonSubstringLength(tab.history[i], tab.searchTerm);
        if (len > maxLen) {// Found longer match
            maxLen = len;
            bestMatch = tab.history[i];
        }
    }

    if (!bestMatch.empty()) {  // Found a suitable substring match
        tab.searchResult = bestMatch;
        return true;
    }

    // 3. No match found
    tab.searchResult = "";
    return false;
}


int main() {
    // --- MODIFIED: Register signal handlers ---
    signal(SIGINT, SIG_IGN);    // Q9: Main shell ignores Ctrl+C
    signal(SIGTSTP, SIG_IGN);   // Q9: Main shell ignores Ctrl+Z
    signal(SIGCHLD, sigchld_handler); // Q9:  Handle dead children

    if (!setlocale(LC_ALL, "")) { cerr << "Warning: Cannot set locale." << endl; }// Set locale for Unicode / multi-language support ---
    if (!XSupportsLocale()) { cerr << "Warning: X does not support locale." << endl; }// Check X locale support
    

    //Open X11 display connection
    Display* display = XOpenDisplay(NULL);// Open X11 display
    if (display == NULL) { cerr << "Error: Cannot open display." << endl; return 1; }// Error handling
    
    int screen = DefaultScreen(display);//  Get default screen// Q1
    XSetLocaleModifiers(""); // Use locale-specific input methods// Q1
    
    // --- Create main window ---
    // --- Q1: GUI Setup (Window) ---
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 10, 10, 800, 600, 1, BlackPixel(display, screen), WhitePixel(display, screen));// Create window
    
    // --- Select which events to listen for ---
    XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask);
    XStoreName(display, window, "MyTerm");// Set window title
    XMapWindow(display, window);// Map (show) the window

    // --- Setup window close event handling ---
    Atom wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wmDeleteMessage, 1);
    
    // --- Initialize input method (for multi-language input) ---
    XIM im = XOpenIM(display, NULL, NULL, NULL);// Open input method
    XIC ic = XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, window, NULL);// Create input context
    XSetICFocus(ic);// Set input context focus

    //Create graphics context
    GC gc = XCreateGC(display, window, 0, NULL); // Q1
    
     // --- Q1/Q3: GUI Font Setup (for Unicode) ---
    char **missing_charsets; int num_missing_charsets = 0; char *default_string;
    const char* font_name = "-*-fixed-medium-r-normal-*-14-*-*-*-*-*-*-*";// Specify font name
    XFontSet fontSet = XCreateFontSet(display, font_name, &missing_charsets, &num_missing_charsets, &default_string);// Create font set
    if (!fontSet) {//   Fallback to 'fixed' font if specified font fails
        cerr << "Error: Could not create fontset '" << font_name << "', falling back to 'fixed'." << endl;
        fontSet = XCreateFontSet(display, "fixed", &missing_charsets, &num_missing_charsets, &default_string);
        if (!fontSet) {// Fatal error if fallback also fails
             cerr << "Fatal: Could not create any fontset." << endl;
             return 1;
        }
    }
    if (num_missing_charsets > 0) { XFreeStringList(missing_charsets); }// Free missing charsets list if any

    // --- Q1: GUI Tabs (Start with one) ---
    vector<ShellTab> tabs;//    Initialize tabs
    tabs.emplace_back();// Start with one tab
    int activeTabIndex = 0;//   Active tab index
    
    redraw(display, window, gc, fontSet, tabs, activeTabIndex);//   Initial draw

    int x11_fd = ConnectionNumber(display);//   Get X11 connection file descriptor

    while (true) {
        //  Robust SIGCHLD reaping loop
         // --- Q9: Job Control - Asynchronous Zombie Reaping ---
        if (g_sigchld_received) {
            g_sigchld_received = false;
            int status;
            pid_t pid;
            
            // Reap all available zombies
            while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {// Reaped a child
                // Now, check if this PID belongs to a background job
                bool reaped = false;
                for (auto& tab : tabs) {
                    for (auto it = tab.jobs.begin(); it != tab.jobs.end(); ++it) {
                        pid_t job_pgid = it->first;
                        // Check if the reaped pid is part of this job group
                        // This is tricky; a simple check is if pid == pgid
                        // A more robust check is harder without more info.
                        // Let's check the whole group.
                        
                        pid_t result = waitpid(-job_pgid, &status, WNOHANG | WUNTRACED);// Check job group status
                        if (result == -1 && errno == ECHILD) {
                             // Job group is gone
                            tab.textBuffer.push_back("[" + to_string(it - tab.jobs.begin() + 1) + "]+  Done  " + it->second);
                            tab.jobs.erase(it);
                            reaped = true;
                            break;
                        } else if (result > 0 && (WIFEXITED(status) || WIFSIGNALED(status))) {
                             // A process in the group exited. Is it the last one?
                             // We'll assume waitpid(-job_pgid) tells us the state of the group.
                             // If it exited, let's reap it.
                             while(waitpid(-job_pgid, &status, WNOHANG) > 0); // reap all in group
                             
                             tab.textBuffer.push_back("[" + to_string(it - tab.jobs.begin() + 1) + "]+  Done  " + it->second);// Notify user
                             tab.jobs.erase(it);
                             reaped = true;
                             break;
                        }
                    }
                    if(reaped) break;
                }
            }
            redraw(display, window, gc, fontSet, tabs, activeTabIndex);// Redraw after reaping
        }

        // (select() call is unchanged)
        // --- Q1/Q2/Q7: Multiplex X11 events and command output ---
        fd_set in_fds;
        FD_ZERO(&in_fds);// Initialize file descriptor set
        FD_SET(x11_fd, &in_fds);//  Monitor X11 events
        int max_fd = x11_fd;

        for (auto& tab : tabs) {// Monitor output pipes from foreground jobs
            if (tab.outputPipeFd != -1) {// Valid pipe
                FD_SET(tab.outputPipeFd, &in_fds);// Add to set
                if (tab.outputPipeFd > max_fd) max_fd = tab.outputPipeFd;// Update max_fd
            }
        }

        if (select(max_fd + 1, &in_fds, NULL, NULL, NULL) == -1) {
            if(errno == EINTR) continue; // Interrupted by SIGCHLD
            perror("select"); continue;// Error handling
        }

        // X11 Event Handling
        // --- Q1: GUI Event Handling ---
        if (FD_ISSET(x11_fd, &in_fds)) {
            while (XPending(display)) {//   Process all pending X11 events
                XEvent event;
                XNextEvent(display, &event);// Get next event

                if (event.type == Expose) {//   Redraw on expose event
                    redraw(display, window, gc, fontSet, tabs, activeTabIndex);
                } 
                else if (event.type == ButtonPress) {//   Handle mouse clicks
                    int clicked_tab = event.xbutton.x / 100;
                    int plusButtonX = tabs.size() * 100;
                    if (event.xbutton.y < 30) {// Click in tab bar area
                        if ((size_t)clicked_tab < tabs.size()) {
                            activeTabIndex = clicked_tab;// Q1: GUI - Tab switching
                        } else if (event.xbutton.x >= plusButtonX && event.xbutton.x < plusButtonX + 30) {// Clicked '+' button
                            tabs.emplace_back();// Q1: GUI - New Tab button
                            activeTabIndex = tabs.size() - 1;
                        }
                    } else {// Scroll in text area
                        ShellTab& activeTab = tabs[activeTabIndex];// Get active tab
                        if (event.xbutton.button == Button4) {// Scroll up
                            activeTab.scrollOffset = max(0, activeTab.scrollOffset - 1);
                        } else if (event.xbutton.button == Button5) {// Scroll down
                             int max_scroll = max(0, (int)activeTab.textBuffer.size() - 10);
                             activeTab.scrollOffset = min(max_scroll, activeTab.scrollOffset + 1);
                        }
                    }
                    redraw(display, window, gc, fontSet, tabs, activeTabIndex);//   Redraw after click// Q1
                }
                else if (event.type == SelectionNotify) {//   Handle clipboard paste
                    if (event.xselection.property) {//   Valid property
                        Atom target;
                        int format;
                        unsigned long nitems, bytes_after;
                        unsigned char *prop_return = NULL;
                        XGetWindowProperty(display, window, event.xselection.property, 0, 1024, False, AnyPropertyType,
                                           &target, &format, &nitems, &bytes_after, &prop_return);
                        if (prop_return) {//   Got clipboard data
                            tabs[activeTabIndex].currentCommand.append((char*)prop_return);
                            XFree(prop_return);
                        }
                        XDeleteProperty(display, window, event.xselection.property);
                    }
                }
                else if (event.type == KeyPress) {//   Handle key presses
                    KeySym keysym; Status status; char buffer[32];
                    int len = Xutf8LookupString(ic, &event.xkey, buffer, sizeof(buffer) - 1, &keysym, &status);
                    buffer[len] = '\0';
                    
                    bool ctrlPressed = event.xkey.state & ControlMask;
                    bool shiftPressed = event.xkey.state & ShiftMask;
                    ShellTab& activeTab = tabs[activeTabIndex];
                    
                    if (activeTab.isForeground) {//   Foreground job running
                        if (ctrlPressed && (keysym == XK_c || keysym == XK_C)) {
                             if (activeTab.foreground_pgid != 0) {
                                kill(-activeTab.foreground_pgid, SIGINT); 
                                activeTab.textBuffer.push_back("^C");
                            }
                        }
                        else if (ctrlPressed && (keysym == XK_z || keysym == XK_Z)) {//   Send SIGTSTP to foreground job
                            // --- Q9: Stop running command (Ctrl+Z) ---
                            if (activeTab.foreground_pgid != 0) {
                                if (kill(-activeTab.foreground_pgid, SIGTSTP) == 0) {// Success
                                    activeTab.jobs.emplace_back(activeTab.foreground_pgid, activeTab.currentForegroundCommand);
                                    activeTab.textBuffer.push_back("[" + to_string(activeTab.jobs.size()) + "]+  Stopped  " + activeTab.currentForegroundCommand);
                                    activeTab.isForeground = false;
                                    activeTab.foreground_pgid = 0;
                                    activeTab.currentForegroundCommand.clear();
                                    if(activeTab.outputPipeFd != -1) {// Close output pipe
                                        close(activeTab.outputPipeFd);
                                        activeTab.outputPipeFd = -1;
                                    }
                                } else {
                                    perror("kill (SIGTSTP)");// Error handling
                                }
                            }
                        }
                        else {
                            continue; // Ignore other keys
                        }
                        redraw(display, window, gc, fontSet, tabs, activeTabIndex);//   Redraw after handling
                        continue; 
                    }
                    
                    if (activeTab.isSearching) {//  In incremental search mode
                        // --- Q10: Handle keypresses during history search ---
                        if (keysym == XK_Return) {//   Finalize search on Enter
                            activeTab.isSearching = false;
                            //Use modified finalizeSearch
                            if (finalizeSearch(activeTab)) {// Found a match// Q10
                                activeTab.currentCommand = activeTab.searchResult;
                                activeTab.cursorPosition = activeTab.currentCommand.length();
                            } else {//  No match found
                                activeTab.textBuffer.push_back("No match for search term in history");// Q10 (Spec 10e)
                                activeTab.currentCommand = "";
                                activeTab.cursorPosition = 0;
                            }
                        } else if (ctrlPressed && (keysym == XK_r || keysym == XK_R)) {//   Next match on Ctrl+R
                            performSearch(activeTab);//   Search again// Q10
                        } else if (keysym == XK_BackSpace) {//   Handle backspace
                            if (!activeTab.searchTerm.empty()) {//   Remove last char
                                activeTab.searchTerm.pop_back();
                                activeTab.searchHistoryIndex = -1; 
                                activeTab.searchResult.clear(); //Clear old result
                                performSearch(activeTab);// Q10
                            }
                        } else if (len > 0) {//   Add char to search term
                            activeTab.searchTerm += buffer;
                            activeTab.searchHistoryIndex = -1;
                            activeTab.searchResult.clear(); //Clear old result
                            performSearch(activeTab);//   Perform search// Q10
                        }
                        redraw(display, window, gc, fontSet, tabs, activeTabIndex);//   Redraw after search update
                        continue;
                    }

                    if(activeTab.isChoosingCompletion){//   In completion choice mode
                        if(len > 0 && isdigit(buffer[0])){//   Digit pressed
                            int choice = atoi(buffer) - 1;//   Convert to index
                            if(choice >= 0 && (size_t)choice < activeTab.completionChoices.size()){//   Valid choice
                                string chosen = activeTab.completionChoices[choice];
                                size_t last_space = activeTab.currentCommand.rfind(' ');//  Replace last word
                                if (last_space == string::npos) activeTab.currentCommand = chosen;// No spaces
                                else activeTab.currentCommand = activeTab.currentCommand.substr(0, last_space + 1) + chosen;// Replace last word
                                activeTab.cursorPosition = activeTab.currentCommand.length();//  Move cursor to end
                            }
                        }
                        activeTab.isChoosingCompletion = false; 
                        redraw(display, window, gc, fontSet, tabs, activeTabIndex);//   Redraw after choice
                        continue;
                    }

                    if (ctrlPressed && shiftPressed && (keysym == XK_v || keysym == XK_V)) {//   Paste from clipboard on Ctrl+Shift+V
                        // --- Q10: Start history search (Ctrl+R) (Spec 10c) ---
                        Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
                        Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);
                        XConvertSelection(display, clipboard, utf8_string, clipboard, window, CurrentTime);// Request clipboard content
                    }
                    else if (ctrlPressed && (keysym == XK_r || keysym == XK_R)) {//   Start incremental search on Ctrl+R
                        activeTab.isSearching = true;
                        activeTab.searchTerm.clear();
                        activeTab.searchResult.clear();
                        activeTab.searchHistoryIndex = -1;// Reset search index
                    }
                    else if (ctrlPressed && (keysym == XK_c || keysym == XK_C)) {//   Handle Ctrl+C
                        if (activeTab.isMultiWatchRunning) {//   Stop multi-watch
                            g_interrupt_flag = true; // Q7: Stop multiWatch (Ctrl+C)
                            activeTab.textBuffer.push_back("^C");// Indicate interrupt
                        } else if (!activeTab.currentCommand.empty()) {//   Clear current command
                            activeTab.currentCommand.clear();
                            activeTab.cursorPosition = 0;
                            activeTab.textBuffer.push_back("user@myterm> ^C"); // Indicate interrupt
                        }
                    } 
                    else if (ctrlPressed && (keysym == XK_z || keysym == XK_Z)) {//   Handle Ctrl+Z
                        // No-op (handled above) // Q9
                    }
                    else if (shiftPressed && keysym == XK_Up) {//   Scroll up with Shift+Up
                        // --- Q10: History navigation (Up/Down arrows) ---
                        activeTab.scrollOffset = max(0, activeTab.scrollOffset - 1);// Scroll up
                    } else if (shiftPressed && keysym == XK_Down) {//   Scroll down with Shift+Down
                        // --- Q10: History navigation (Up/Down arrows) ---
                        int max_scroll = max(0, (int)activeTab.textBuffer.size() - 10);
                        activeTab.scrollOffset = min(max_scroll, activeTab.scrollOffset + 1);
                    } 
                    else if (keysym == XK_Up) {//   Navigate command history up
                        if (activeTab.historyIndex > 0) {//  Can go up
                            activeTab.historyIndex--;
                            activeTab.currentCommand = activeTab.history[activeTab.historyIndex];
                            activeTab.cursorPosition = activeTab.currentCommand.length();
                        }
                    }
                    else if (keysym == XK_Down) {//   Navigate command history down
                        if (activeTab.historyIndex < (int)activeTab.history.size() - 1) {// Can go down
                            activeTab.historyIndex++;
                            activeTab.currentCommand = activeTab.history[activeTab.historyIndex];
                        } else {// At the end of history
                            activeTab.historyIndex = activeTab.history.size();
                            activeTab.currentCommand = "";
                        }
                        activeTab.cursorPosition = activeTab.currentCommand.length();// Move cursor to end
                    }
                    else if (ctrlPressed && keysym == XK_n) {//   New tab on Ctrl+N
                        tabs.emplace_back();// Q1: GUI - New Tab
                        activeTabIndex = tabs.size() - 1;
                    } else if (keysym == XK_Left) {//   Move cursor left
                        if (activeTab.cursorPosition > 0) activeTab.cursorPosition--;
                    } else if (keysym == XK_Right) {//   Move cursor right
                        if (activeTab.cursorPosition < (int)activeTab.currentCommand.length()) activeTab.cursorPosition++;
                    } else if (ctrlPressed && (keysym == XK_a || keysym == XK_A)) {//   Move cursor to start on Ctrl+A
                        activeTab.cursorPosition = 0;// Q8: Line Navigation (Ctrl+A - Start)
                    } else if (ctrlPressed && (keysym == XK_e || keysym == XK_E)) {//   Move cursor to end on Ctrl+E
                        activeTab.cursorPosition = activeTab.currentCommand.length();// Q8: Line Navigation (Ctrl+E - End)
                    }
                    else if (keysym == XK_Tab) {//   Handle tab completion// --- Q11: Auto-complete (Tab) ---
                        string word_to_complete = get_word_before_cursor(activeTab.currentCommand, activeTab.cursorPosition);
                        if (!word_to_complete.empty()) {
                            glob_t glob_result;
                            string pattern = word_to_complete + "*";
                            glob(pattern.c_str(), GLOB_TILDE, NULL, &glob_result);
                            if (glob_result.gl_pathc == 1) {//   Single match
                                // --- Q11: (Spec 11.i) Single match ---
                                string completion = glob_result.gl_pathv[0];
                                size_t last_space = activeTab.currentCommand.rfind(' ');
                                if (last_space == string::npos) activeTab.currentCommand = completion;
                                else activeTab.currentCommand = activeTab.currentCommand.substr(0, last_space + 1) + completion;
                                activeTab.cursorPosition = activeTab.currentCommand.length();
                            } else if (glob_result.gl_pathc > 1) {//   Multiple matches
                                vector<string> matches;
                                for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
                                    matches.push_back(glob_result.gl_pathv[i]);
                                }
                                string lcp = find_longest_common_prefix(matches);// Q11
                                if (lcp.length() > word_to_complete.length()) {//   Extend current input
                                    size_t last_space = activeTab.currentCommand.rfind(' ');
                                     if (last_space == string::npos) activeTab.currentCommand = lcp;
                                     else activeTab.currentCommand = activeTab.currentCommand.substr(0, last_space + 1) + lcp;
                                     activeTab.cursorPosition = activeTab.currentCommand.length();
                                } else {//   Show choices// --- Q11: (Spec 11.iii) Multiple matches - show choices -
                                    activeTab.completionChoices = matches;
                                    activeTab.isChoosingCompletion = true;
                                    activeTab.textBuffer.push_back("user@myterm> " + activeTab.currentCommand);//   Show current command
                                    for(size_t i=0; i<matches.size(); ++i){
                                        activeTab.textBuffer.push_back(to_string(i+1) + ". " + matches[i]);//   List choices
                                    }
                                }
                            }// Q11: (Spec 11.iv) is implicit (no match -> do nothing)
                            globfree(&glob_result);//       Free glob result
                        }
                    }
                    else if (keysym == XK_Return) {//   Execute command on Enter
                        activeTab.scrollOffset = max(0, (int)activeTab.textBuffer.size() - 10);//  Scroll to bottom
                        string& cmd = activeTab.currentCommand;
                        size_t endpos = cmd.find_last_not_of(" \t");// Trim trailing whitespace
                        if (string::npos != endpos) cmd = cmd.substr(0, endpos + 1);// Trimmed
                        else cmd.clear();
                        if (!cmd.empty() && cmd.back() == '\\') { // --- Q3: Handle multiline input ---
                            cmd.pop_back(); 
                            activeTab.multiLineCommand += cmd + " ";
                            activeTab.textBuffer.push_back("user@myterm> " + cmd + "\\");
                            cmd.clear();
                        } else {//   Final command
                            if (!cmd.empty() || !activeTab.multiLineCommand.empty()) {
                                executeCommand(display, window, gc, fontSet, tabs, activeTabIndex);
                            }
                            cmd.clear();
                        }
                        activeTab.cursorPosition = 0;//   Reset cursor position
                    } else if (keysym == XK_BackSpace) {//   Handle backspace// --- Q1: Basic line editing ---
                        if (activeTab.cursorPosition > 0) {
                            activeTab.currentCommand.erase(activeTab.cursorPosition - 1, 1);
                            activeTab.cursorPosition--;
                        }
                    } else if (len > 0 && keysym != XK_Tab) {//   Regular character input (excluding Tab)
                                                // --- Q1/Q3: Handle regular character input ---
                        activeTab.currentCommand.insert(activeTab.cursorPosition, buffer);
                        activeTab.cursorPosition += len;
                        activeTab.historyIndex = activeTab.history.size();
                    }
                    redraw(display, window, gc, fontSet, tabs, activeTabIndex);//   Redraw after key press
                } else if (event.type == ClientMessage) {
                    if ((Atom)event.xclient.data.l[0] == wmDeleteMessage) goto end_loop;
                }
            }
        }

        // --- Command Output Handling  ---
        // --- Q1/Q2/Q6: Handle Command Output ---
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i].outputPipeFd != -1 && FD_ISSET(tabs[i].outputPipeFd, &in_fds)) {
                char read_buffer[4096];
                ssize_t count = read(tabs[i].outputPipeFd, read_buffer, sizeof(read_buffer) - 1);

                if (count > 0) { // Read successful 
                    read_buffer[count] = '\0';
                    stringstream ss(read_buffer);
                    string line;
                    while(getline(ss, line)) {//   Process each line
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        tabs[i].textBuffer.push_back(line);
                    }
                    tabs[i].scrollOffset = max(0, (int)tabs[i].textBuffer.size() - 10);
                } 
                else { // Pipe closed (command finished)
                    // --- Q2/Q9: Command finished, cleanup ---
                    close(tabs[i].outputPipeFd);
                    tabs[i].outputPipeFd = -1;
                    
                    // Wait for all children in the foreground group to finish
                    for(pid_t pid : tabs[i].childPids) {
                         waitpid(pid, NULL, 0); 
                    }
                    tabs[i].childPids.clear(); 
                    
                    // CRITICAL FIX: Reset foreground state
                    // --- Q9: Reset foreground state ---
                    tabs[i].isForeground = false;
                    tabs[i].foreground_pgid = 0;
                    tabs[i].currentForegroundCommand.clear();
                }
                
                if ((int)i == activeTabIndex) {//   Redraw if active tab
                    redraw(display, window, gc, fontSet, tabs, activeTabIndex);
                }
            }
        }
    } // end while(true)

end_loop:
// --- Q1/Q9: Cleanup before exit ---
    for(const auto& tab : tabs) {//   Cleanup before exit
        for(pid_t pid : tab.childPids) {
            if (pid > 0) kill(pid, SIGKILL);//  Kill remaining child processes
        }
         for(const auto& job : tab.jobs) {// Kill remaining jobs
            if (job.first > 0) kill(-job.first, SIGKILL); // Kill remaining jobs
        }
    }

    if (fontSet) { XFreeFontSet(display, fontSet); }// Free font set// Q1
    if (ic) { XDestroyIC(ic); }// Destroy input context// Q1/Q3
    if (im) { XCloseIM(im); }// Close input method// Q1
    XFreeGC(display, gc);// Free graphics context// Q1
    XDestroyWindow(display, window);// Destroy window
    XCloseDisplay(display);// Close display connection// Q1
    return 0;
}
