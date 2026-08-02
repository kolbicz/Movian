include ${BUILDDIR}/config.mak

build:
	cmake --build ${LIBSMB2_BUILD_DIR} --parallel
	cmake --install ${LIBSMB2_BUILD_DIR}
