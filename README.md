# Interpolating format strings in LLDB prompt

This summarizes changes proposed to enabled interpolating format strings in the prompt.

# Feature motivation

Currently there is a very useful ability to specify format strings with placeholders to customize output in a few places:

* Backtrace (`settings show thread-format`)
* Thread stop output (`settings show thread-stop-format`)
* Frame info (`settings show frame-format`)
* Frame info for unique backtrace command (`settings show frame-format-unique`)
* ...

The entire list of available variables & formatting code is [here](lldb/source/Core/FormatEntity.cpp).

The proposed feature is to support interpolating a format string specified for the prompt, similar to `PS1` in shells, using the existing infrastructure for defining variables and interpolating format strings.

# Background

Console I/O is handled via [`IOHandler`](lldb/source/core/IOHandler.cpp), which is an abstract base class.  There are a few different subclasses:

* `IOHandlerEditline`, which is the main one, and handles most input, including multiline (e.g. entering a list of commands for execution at a breakpoint) It also contains logic for using EditLine/libedit if it is available and falling back if not.
* `IOHandlerConfirm` (inherits from `IOHandlerEditline`), which is used for prompting the user for Y/N input and supports things like a default answer if the user just pushes enter. 
* IOHandlers for the Python interpreter, target I/O, and a few others.  Some pass different `IOHandler::Type` values to the `IOHandlerEditline` constructor, and some are subclasses.

There's also a delegate interface that IOHandler uses to notify of events.  CommandInterpreter implements this interface.

## Flow of user input 

An `IOHandler` gathers input from the user using standard library routines and provides an output stream for command output when its `Run()` method is called.  Command execution is initiated through the IOHandlerDelegate interface, a primary implementation of which is inside `CommandInterpreter.cpp`, and another implementation is in `SBCommandInterpreter` (difference seems to be SB* is used for API implementation?)

When `CommandInterpreter::Run` is called (TODO: from where besides SBCommandInterpreter?), it starts the following chain of function calls:

1. `Debugger::StartIOHandlerThread`
2. `Debugger::RunIOHandlers`
3. `RunIOHandlers` pushes an IO Handler onto stack, cancels existing top IO handler, and runs the new one

(specifics of managing IO Handler Stack omitted because ~~I don't kno~~ of time constraints)

The stack is used for commands to push a new input handler to read more from the user when this is necessary.  For instance, "quit" will push an IOHandlerConfirm object onto the stack to read the user's confirmation choice.  Commands such as "break command" will push an IOHandlerEditline with the `multiline` parameter set to `true` to read the commands to be executed when that breakpoint is hit.

 # Prompt handling
 
When `CommandInterpreter` constructs an `IOHandlerEditline`, it calls `Debugger::GetPrompt()` (on it's instance member of type `Debugger`, not a static method), which retrieves the prompt string from the user settings.  `IOHandlerEditline` stores the prompt locally and also calls into libedit to set the prompt there.  This logic is also how the prompt is retrieved from `IOHandlerEditline`.

## Editline prompt callback support

Editline provides an interface with callbacks for certain scenarios.  Currently LLDB takes advantage of those in a few ways: to tell EditLine when input is done in multiline scenarios (`CommandObjectExpression::IOHandlerIsInputComplete`), to fix indentation for REPL input, for auto suggest, etc.  Editline also provides a facility to return a prompt via a callback that a client provides.  To do this, we can configure the editline instance with a callback for the `EL_PROMPT` parameter to `el_set`. 

## Proposed changes

By calling into the `FormatEntity` class inside either `IOHandler` or `Debugger` when the prompt is retrieved, we can have it contain updated information on the target for every prompt.  We can do this in both any editline callback, as well as in the non-editline case every time the prompt is printed.
