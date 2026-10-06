#!/bin/sh
# Make the netscape5 tardist (an inst(1M) distribution in a tar file) for
# IRIX 6.5.22 and later.  Runs on IRIX, with gendist(1M) (inst_dev, the
# Software Packager).
#
#   make-tardist.sh NETSCAPE5 CACERT.PEM FONTS VERSION [OUTDIR]
#
# NETSCAPE5   the program: www/netscape5 built with the irix-motif option
#             (IRIX's own Motif and X, OpenSSL linked in) for n32-mips3, so
#             that it runs on every IRIX 6.5 machine
# CACERT.PEM  Mozilla's roots of trust (pkgsrc security/mozilla-rootcerts:
#             share/mozilla-rootcerts/cacert.pem)
# FONTS       a directory with the page fonts: croscorefonts' Arimo, Tinos
#             and Cousine (12 files) and DejaVuSans.ttf (pkgsrc's
#             share/fonts/X11/TTF has them all)
# VERSION     the image version, a number that grows with each release
#             (for example 500010001: 5.0b1, build 1)
#
# Installs /usr/local/bin/netscape5 and /usr/local/lib/netscape5 (roots,
# fonts, licenses, README).  The program finds its fonts in
# /usr/local/lib/netscape5/fonts (cmd/xfe/ftfonts.c).  The program reads its roots from
# /usr/local/lib/netscape5/cacert.pem (NETSCAPE_CA_FILE in the recipe).
set -eu
# IRIX's /bin/sh is a Bourne shell: backquotes, not $(...).
here=`dirname "$0"`
here=`cd "$here" && pwd`
top=`cd "$here/../.." && pwd`
prog=$1 cacert=$2 fonts=$3 version=$4 out=${5:-`pwd`}
case $version in *[!0-9]*|"") echo "VERSION must be a number" >&2; exit 1 ;; esac

work=/usr/tmp/netscape5-dist.$$
trap 'rm -rf "$work"' 0
mkdir -p "$work/src/usr/local/bin" "$work/src/usr/local/lib/netscape5/licenses" \
	"$work/src/usr/local/lib/netscape5/fonts" "$work/dist"

cp "$prog" "$work/src/usr/local/bin/netscape5"
cp "$cacert" "$work/src/usr/local/lib/netscape5/cacert.pem"
cp "$here/README" "$work/src/usr/local/lib/netscape5/README"
cp "$top/LICENSE" "$work/src/usr/local/lib/netscape5/licenses/NPL-1.0"
cp "$here/licenses/"* "$work/src/usr/local/lib/netscape5/licenses/"
for f in Arimo Tinos Cousine; do
	for s in Regular Bold Italic BoldItalic; do
		cp "$fonts/$f-$s.ttf" "$work/src/usr/local/lib/netscape5/fonts/"
	done
done
cp "$fonts/DejaVuSans.ttf" "$work/src/usr/local/lib/netscape5/fonts/"

# The idb: type mode owner group destination source tags, sorted on the
# destination and source (gendist(1M)).
(
	cd "$work/src"
	for d in usr/local/lib/netscape5 usr/local/lib/netscape5/fonts \
		usr/local/lib/netscape5/licenses; do
		echo "d 0755 root sys $d $d NETSCAPE5_BASE"
	done
	echo "f 0755 root sys usr/local/bin/netscape5 usr/local/bin/netscape5 NETSCAPE5_BASE nostrip"
	find usr/local/lib/netscape5 -type f -print | while read f; do
		echo "f 0644 root sys $f $f NETSCAPE5_BASE"
	done
) | sort +4u -6 > "$work/idb"

sed "s/VERSION/$version/" "$here/netscape5.spec" > "$work/spec"

/usr/sbin/gendist -rbase / -sbase "$work/src" -idb "$work/idb" \
	-spec "$work/spec" -dist "$work/dist" -nostrip -all

name=netscape5-5.0b1-$version.tardist
(cd "$work/dist" && tar cf - netscape5*) > "$out/$name"
ls -l "$out/$name"
