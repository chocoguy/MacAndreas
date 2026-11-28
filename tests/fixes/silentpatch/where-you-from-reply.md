# 'Where are you from' replies

Group `where-you-from-reply` (byte_patches at 0x43bc13 and 0x43be14, call_redirect at 0x43c0c7).

**Bug.** After CJ's positive answer, hostile gangsters don't respond.

**Port.** As SilentPatch, both branches jump to the reply path. The reply says
WHERE_YOU_FROM_POS_REPLY for that topic, and nothing for gang-banging.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
