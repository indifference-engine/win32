CC = x86_64-w64-mingw32-gcc
CFLAGS = -Wall -Wextra -Werror -std=c99 -nostdlib -ffreestanding -O3 -pedantic -ffp-contract=off

# Unfortunately, I have found that make quite often selects the wrong shell
# (e.g. PowerShell), so commands like "find" won't work unless we explicitly
# specify bash.  You also can't use a variable for this (e.g. $(SHELL)) as make
# inexplicably tries to read something from the PATH and fails.  So hardcoding a
# reference to bash seems to be the only way to get a working build.
C_FILES = $(shell bash -c "find src -type f -iname ""*.c""")
H_FILES = $(shell bash -c "find src -type f -iname ""*.h""")
O_FILES = $(patsubst src/%.c,obj/%.o,$(C_FILES))
TOTAL_REBUILD_FILES = makefile $(H_FILES)

dist/example.exe: $(O_FILES) obj/resource.res
	mkdir -p $(dir $@)
	$(CC) $(CLAGS) -flto -mwindows $(O_FILES) obj/resource.res -o $@ -ldwmapi -lwinmm

obj/%.o: src/%.c $(TOTAL_REBUILD_FILES)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

obj/resource.res: src/example/resource.rc src/example/manifest.manifest makefile
	mkdir -p $(dir $@)
	windres $< -O coff -o $@

clean:
	rm -rf obj dist
