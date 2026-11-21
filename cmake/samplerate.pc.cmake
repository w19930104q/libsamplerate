# CMake-specific pkg-config template. The autotools build keeps its own
# samplerate.pc.in untouched.
#
# The absolute "prefix=" line keeps the file usable with pkg-config versions
# older than 0.20 (which do not provide pcfiledir); every path is then
# re-derived relative to the directory containing this .pc file, so the whole
# installation tree can be moved and pkg-config still resolves correctly.
prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
libdir=${pcfiledir}/@PC_RELATIVE_LIBDIR@
includedir=${pcfiledir}/@PC_RELATIVE_INCLUDEDIR@

Name: samplerate
Description: An audio Sample Rate Conversion library
Requires:
Version: @PROJECT_VERSION@
Libs: -L${libdir} -lsamplerate
Libs.private: @PC_LIBS_PRIVATE@
Cflags: -I${includedir}
