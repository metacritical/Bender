</$objtype/mkfile

TARG=bender
HFILES=src/include/bender.h src/conf/benderconf.h
BENDER_PATH=/sys/lib/bender
BIN=/$objtype/bin/
DISABLED=EV NET ASSEMBLER FFI UTC_MKTIME REALPATH DYNAMIC_MODULES THREADS SYMLINKS LOCALES UMASK SPAWN
BENDER_CONFIG=BENDER_SINGLE_THREADED BENDER_OS_NAME=plan9 BENDER_ARCH_NAME=$objtype BENDER_API='' BENDER_NO_RETURN='' BENDER_SIMPLE_GETLINE `{echo BENDER_NO_^$DISABLED} PLAN9_`{echo -n $objtype}
CFLAGS=-FTVBNcwp -D _POSIX_SOURCE -DBENDER_PLAN9 -D_BSD_EXTENSION -D_LIMITS_EXTENSION -Isrc/include -Isrc/conf -I/sys/include/npe -Dtypestr=bendertypestr -DBENDER_API `{echo '-D'^$BENDER_CONFIG}
BOOT_CFLAGS=$CFLAGS -DBENDER_BOOTSTRAP

BENDER_CORE_HEADERS=`{ls src/core/*.h}
BENDER_CORE_SOURCES=`{ls src/core/*.c}

BENDER_BOOT_SOURCES=src/boot/array_test.c \
				   src/boot/boot.c \
				   src/boot/buffer_test.c \
				   src/boot/number_test.c \
				   src/boot/system_test.c \
				   src/boot/table_test.c
BENDER_BOOT_HEADERS=src/boot/tests.h
BENDER_BOOT_OBJECTS=`{echo $BENDER_CORE_SOURCES $BENDER_BOOT_SOURCES | sed -e 's/\.c/.boot.'$O'/g'}

OFILES=bender.$O src/mainclient/shell.$O

default:V:all

src/%.boot.$O: src/%.c $BENDER_HEADERS $BENDER_CORE_HEADERS $BENDER_BOOT_HEADERS
	$CC $BOOT_CFLAGS  -o $target $prereq(1)

src/mainclient/shell.$O: src/mainclient/shell.c
	$CC $BOOT_CFLAGS  -o $target $prereq(1)

$O.benboot: $BENDER_BOOT_OBJECTS
	$LD $LDFLAGS -o $target $prereq

bender.c: $O.benboot src/boot/boot.ben
	$prereq(1) . BENDER_PATH $BENDER_PATH >$target

build/bender.$O: build/c/bender.c src/conf/benderconf.h src/include/bender.h
	$CC $CFLAGS -D^$BENDER_CONFIG -o $target $prereq(1)

build/shell.$O: src/mainclient/shell.c src/conf/benderconf.h src/include/bender.h
	$CC $CFLAGS -D^$BENDER_CONFIG -o $target $prereq(1)

</sys/src/cmd/mkone

clean:V:
	rm -f src/core/*.[$OS] src/boot/*.[$OS] *.a[$OS] y.tab.? lex.yy.c y.debug y.output [$OS].??* $TARG bender.[$OS] bender.c src/mainclient/shell.[$OS]
