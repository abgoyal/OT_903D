# Copyright (C) 1999-2009, International Business Machines  Corporation and others.  All Rights Reserved.
s%^\([a-zA-Z\._-]*\)[ 	]*+=%\1=$(\1) %
s%^[A-Z]*_SO_TARG*%## &%
s%^SHARED_OBJECT.*%## &%
s%^LD_SONAME.*%## &%
s%$(\([^\)]*\))%${\1}%g
s%^	%#M#	%
s@^[a-zA-Z%$.][^=]*$@#M#&@
s@^\([a-zA-Z][-.a-zA-Z_0-9-]*\)[	 ]*=[ 	]*\(.*\)@\1="\2"@
s@^\([a-zA-Z][-a-zA-Z_0-9-]*\)\.\([a-zA-Z_0-9-]*\)[	 ]*=[ 	]*\(.*\)@\1_\2=\3@
s@^\([a-zA-Z][-a-zA-Z_0-9-]*\)\-\([a-zA-Z_0-9-]*\)[	 ]*=[ 	]*\(.*\)@\1_\2=\3@
s@\${\([a-zA-Z][-a-zA-Z_0-9-]*\)\.\([a-zA-Z_0-9-]*\)}@${\1_\2}@g
s@^\(prefix\)=\(.*\)@default_\1=\2\
if [ "x${\1}" = "x" ]; then \1="$default_\1"; fi@
s@^\(ENABLE_RPATH\)=\(.*\)@default_\1=\2\
if [ "x${\1}" = "x" ]; then \1="$default_\1"; fi@
s%^#SH#[ ]*%%
