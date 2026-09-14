# Wisconsin node expired

The user confirmed on September 7 that the Wisconsin node expired. Two SSH
checks timed out and the old experiment alias no longer resolved. Historical
metadata/status files remain unchanged snapshots; they do not describe a
currently running queue.

The last live experiment snapshot was September 7 at 00:55:29 UTC. The 16B
entry result was retrieved and validated: six candidates, 100 measurements
each, all requested paths correct. Its best h3/h1 throughputs are 1023.275
and 879.883 MiB/s.

The 48B, 64B, 128B and Wisconsin t257 final outcomes were not retrieved.
They may have progressed after the last snapshot, but completion cannot be
claimed from the files available locally. This is not evidence of a
decryption failure or proof that every remote result was lost.

The portal shows a new Wisconsin experiment zcy05331-314863 as scheduled
with zero allocated nodes. No new experiment was created or reservation
submitted during this status check.
