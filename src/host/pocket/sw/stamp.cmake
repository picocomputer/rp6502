# Writes OUT, the header that gives crt0.S the firmware's build stamp: the
# time of this link, in seconds since 1970. sw.cmake runs it in the command
# that links the firmware, so the stamp changes when the firmware does and
# at no other time.
string(TIMESTAMP now "%s" UTC)
file(WRITE ${OUT} "#define BUILD_STAMP ${now}\n")
