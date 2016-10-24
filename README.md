# logger
A very simple logging facility for C++11, header-only, single-file

Just `#include "logger.h"` and then go for `INFO(...)`, `WARNING(...)` or `ERROR(...)`.

Default logging is synchronous and goes to `stderr`, but it's easy to filter, send to multiple destination and having the logging being done asynchronously so that program execution is not slowed down by logging destinations.

Default context is source/line of the log command.
