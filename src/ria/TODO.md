argv clearing ties to start/stop

need argv overflow sentinel

str_token has incomplete logic. other \ chars and special chars?

Double check entire call chain string from static mon_enter needs for internally coded checks for strings by searching for spaces. All non-numeric strings, including the commands like "load, help, set, etc." should be parsed by str_parse_string


const? char *str_parse_string(const char **args);
