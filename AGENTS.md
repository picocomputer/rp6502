It's always worth the tokens to read ~/picocomputer.github.io at the start
of every session.

This is a RP2350 project, not RP2040 as some legacy filenames may suggest.

A machine is everything that comes together to make a Picocomputer.
src/host brings together the core with the needed services.
src/osal connects the core with an operating system.
src/core is the bulk of what makes a Picocomputer.

We do not use the TinyUSB in the Pi Pico SDK.
We have overrides for many of our submodules. See: vendor/*_rp6502

We do not waste memory defending against bad code. Every byte is precious but
it's always better to fix bad code even if it's a few more bytes than a hack.

Do not commit or push unless specifically asked to. Do not look for answers
in git history unless asked to.

Comments. Do not narrate the session in comments. A comment states the
purpose of the code in one or two short sentences.

Voice. Write for engineers and programmers, in the style of a data sheet.
This applies to the manual, comments, messages, help text, tests and plans.
Use plain language with the writing rules of ASD-STE100 Simplified Technical
English, but not its restricted dictionary:
- Use one word for one meaning, and use the same word every time.
- Use the present tense and the active voice. Give each sentence a subject.
- Prefer short sentences. A sentence is as long as its content requires.
- Write one instruction in each sentence.
- Do not use noun clusters of more than three words.
- Do not use figures of speech, idioms or decoration.

Use the RFC 2119 words with their RFC meanings. "Must" and "must not" state
a requirement. "Should" and "should not" state a recommendation. "May"
states an option. "Can" states an ability. A command that does not run
without an argument requires that argument.

Machines and software are not beings. They have no motives, feelings or
personalities. State what they do as fact: "load replaces the symbols" and
"poke requires an address". Use the established term of the field, such as
"request" for an interrupt request.
