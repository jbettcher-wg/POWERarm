# POWERarmEnv: link-time FEX_* -> POWERARM_* environment translation.
#
# The tree carries a few hundred raw getenv("FEX_...") calls, most of them in the
# PPC64LE JIT and IR passes that are shared with fastppcx86 by cherry-pick. Rather
# than editing every call site (and silently missing the ones future cherry-picks
# add), the linker redirects getenv/secure_getenv/setenv/unsetenv to wrappers that
# rewrite a leading "FEX_" to POWERARM_ENV_PREFIX. Code that builds or scans raw
# environment strings (Config.cpp's EnvLoader, the execve FD hand-off) uses
# POWERARM_ENV_PREFIX directly.
#
# Consumers get the --wrap flags through INTERFACE_LINK_OPTIONS; "-u" forces the
# wrapper object out of the archive regardless of where it lands on the link line.
add_library(POWERarmEnv STATIC EnvPrefix.cpp)
set(POWERARM_WRAPPED_ENV_FUNCS getenv secure_getenv setenv unsetenv)
foreach(Func ${POWERARM_WRAPPED_ENV_FUNCS})
  target_link_options(POWERarmEnv INTERFACE "LINKER:--wrap=${Func}")
endforeach()
target_link_options(POWERarmEnv INTERFACE "LINKER:-u,__wrap_getenv")
