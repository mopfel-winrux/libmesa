/// @file

#ifndef MESA_LOG_H
#define MESA_LOG_H

/* mesa_log_f: receives one formatted log line (no trailing newline).
*/
typedef void (*mesa_log_f)(void* ctx, const char* line);

/* mesa_set_log(): route library logging to [fun]; NULL restores stderr.
**
**   process-global; set it before creating any mesa instance.
*/
void
mesa_set_log(mesa_log_f fun, void* ctx);

#endif /* ifndef MESA_LOG_H */
