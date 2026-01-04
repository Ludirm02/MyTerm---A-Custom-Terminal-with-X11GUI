# This readme is about how to run the project 

MyTerm – Custom X11 Terminal

Description

MyTerm is a custom Linux terminal built using the X11 graphics library.
It provides a GUI interface for command execution, supports multiple tabs, command history, 
piping, redirection, 
and advanced features like multiWatch, auto-completion, and signal handling (Ctrl+C, Ctrl+Z, Ctrl+A,
 Ctrl+E, etc).
===================================================================================================


Compilation
For compiling the myterm.cpp project file we use :

g++ myterm.cpp -o op -lX11
========================================================================================


Running the Program
For running purpose we use 

./op
=========================================================================================

Example Usage
user@myterm> ls -l
user@myterm> ./a.out < input.txt > output.txt
user@myterm> ls | wc -l
user@myterm> multiWatch ["date", "whoami"]

========================================================================================

Notes:

Use Ctrl+C to stop a running command (not the whole shell).
Use Ctrl+Z to send a command to the background.
Use Ctrl+A / Ctrl+E to move cursor to start/end of line.
Unicode characters are supported with setlocale() in your code.
For tab-based shell instances, each tab runs in a separate forked process.

==========================================================================================
