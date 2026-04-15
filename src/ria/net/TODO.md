we want telnet access to the console.
it should be hooked into com.c tee-style
that means outbound speeds are capped to the UART speed, that's ok
incoming speeds can go full speed, which is what we want
add two settings
 - PORT for the listen port
 - KEY for the authentication passkey (can be anything)
There is no username for the login, but the system will prompt
for the password. it will echo * for each letter and only support
backspace (see rln for all backspace keystrokes).
this passkey prompt will not show up on the system console.
but after auth, everything connected to the console is "tee'd".
it's ok that the passkey is on the wire in plain text.

resolving port conflicts:
it's not possible to change system port while a modem is in use, so ignore these cases
if a modem starts up using the system port, reset it to 0 but don't save.
if a modem attempt to configure \L the system port, give an error.
