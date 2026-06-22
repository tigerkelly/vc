#ifndef VC_DIFF3_H
#define VC_DIFF3_H

// ---------------------------------------------------------------------------
// vcDiff3.h  –  Pure C 3-way merge (diff3-like).
//
// Performs a line-level 3-way merge of three text files:
//   ours   = current branch version
//   base   = common ancestor
//   theirs = incoming branch version
//
// Writes merged result to outPath.
// Returns:
//   0  clean merge — no conflicts
//   1  merged with conflict markers
//  -1  error (file I/O, memory)
//
// Conflict markers follow the standard git/diff3 format:
//   <<<<<<< ours
//   ... our lines ...
//   ||||||| base
//   ... base lines ...
//   =======
//   ... their lines ...
//   >>>>>>> theirs
// ---------------------------------------------------------------------------

int vc_merge3(const char *oursPath,
              const char *basePath,
              const char *theirsPath,
              const char *outPath,
              const char *oursLabel,
              const char *theirsLabel);

#endif // VC_DIFF3_H
