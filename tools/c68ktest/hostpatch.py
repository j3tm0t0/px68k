#!/usr/bin/env python3
# hostpatch.py <tree> : make the SDL host build (x11/winx68k.cpp) able to run
# headless and deterministic, for host.sh.  With HOSTRUN_FRAMES=N it runs N
# frames flat out (every frame drawn, fixed RTC, sound stopped), prints
# "frame N <FNV-1a hash of the screen> WxH" to stderr every 50 frames, exits.
import sys
p = sys.argv[1] + '/x11/winx68k.cpp'
s = open(p, encoding='latin-1').read()
old = """#else
			WinX68k_Exec();
#endif"""
new = """#else
			{
				static int hf = -1;
				static unsigned fno;
				extern WORD *ScrBuf;
				if (hf < 0) {
					char *e = getenv("HOSTRUN_FRAMES");
					hf = e ? atoi(e) : 0;
					if (hf) {
						RTC_TimeHook = host_fixed_time;
						DSound_Stop();
					}
				}
				if (hf) { Config.NoWaitMode = 1; Config.FrameRate = 1; }
				WinX68k_Exec();
				if (hf) {
					fno++;
					if (fno % 50 == 0 || fno == (unsigned)hf) {
						unsigned h = 2166136261u; int x, y;
						for (y = 0; y < TextDotY; y++)
							for (x = 0; x < TextDotX; x++)
								h = (h ^ ScrBuf[y * 800 + x]) * 16777619u;
						fprintf(stderr, "frame %u %08x %dx%d\\n", fno, h, TextDotX, TextDotY);
					}
					if (fno >= (unsigned)hf) exit(0);
				}
			}
#endif"""
assert s.count(old) == 1
s = s.replace(old, new)
old2 = "int main(int argc, char *argv[])\n#endif"
assert s.count(old2) == 1
assert s.count("#define CLOCK_SLICE") == 1
s = s.replace("#define CLOCK_SLICE", """#ifndef PSP
extern "C" time_t (*RTC_TimeHook)(void);
static time_t host_fixed_time(void) { return 946684800; }
#endif
#define CLOCK_SLICE""")
open(p, 'w', encoding='latin-1').write(s)
print("patched", p)
