
./configure --enable-librtmp --disable-indevs --disable-outdevs --extra-version=static \
	--extra-cflags=--static \
	--enable-static \
	--disable-shared --disable-ffplay --disable-ffserver \
	--extra-ldexeflags="-static -ldl"

