# Decryption-only experiment acceptance

The user removed the 1-bit noise-headroom requirement. Generic preflight and
timed runs now default to min_noise_margin=0, explicitly meaning admission
by actual decryption correctness; diagnostic margins may be zero or negative.
Positive explicitly requested thresholds retain their prior numeric policy.
Correct key/tag bindings and actual decoded entry checks remain in place.
The g32/t257 benchmark still checks every response coefficient including unused
slots, but records noise without using it to reject a correct response.

Local validation: 39/39 applicable regression tests plus the CLI regression;
20 Python comparison/entry tests; 16B Generic and g32 encrypted smoke profiles.
The tests cover low diagnostic margins being admitted and wrong decryptions
being rejected. Arithmetic, encoding, modulus choices and timing scopes did
not change. Historical results and failures keep their original acceptance
criteria and are not retroactively relabeled.

Wisconsin restart roots:
- /users/zcy05331/generic-entry-wisconsin-decrypt-20260906
- /users/zcy05331/tree-t257-wisconsin-decrypt-20260906

Stop the old waiting t257 queue before ending the old entry controller.
Preserve old logs. Build the revised Generic binary and rerun 16/48/64/128B
(48B keeps 2^24 leaves; other sizes target 1 GiB). The later t257 controller
waits for this new entry controller, including its build/validation stages.
All cases use 100 measurements and 3 warmups.
