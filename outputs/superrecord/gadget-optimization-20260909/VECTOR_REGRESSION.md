# Native GSW vectorization regression and repair

Initial implementation 5d5f6c3 was mathematically correct but scalarized under
native GCC. Both execution orders reproduced the regression at unchanged
EP6/KEY10/KS8: h3 folding rose from about166ms to284ms and rotation from19ms
to31ms, while scan remained about526ms. Source a9503fb was the control.

Read-only native disassembly found vector centering and digit loops in the
baseline function at0xd63d0 (size0x350), including vpsllq/vpsraq at0xd6580.
The v1 function at0xd8a90 (size0x265) had scalar loops and reloaded low_bits_
at0xd8b45 and base_log2_ at0xd8bcd for each coefficient. No hot idiv/div was
present. The regression was lost vectorization, not integer division.

Commit8506e7f hoists immutable gadget parameters into locals, separates optional
rounding, and restores branchless sign extension with unsigned left shift.
The rounded gadget and its error bound are unchanged. Default41/41 regression
passed after repair. Corrected native assembly/timing validation follows.

native-v1/ is retained as a diagnostic run, including800 correct measured
queries. It is not the final optimized result table.

Final native disassembly recovered vector loops in both binaries. The default
parameter control matches baseline runtime; all600 final native queries passed.
See native-v2 and local-validation/native-vector-fix.txt.
