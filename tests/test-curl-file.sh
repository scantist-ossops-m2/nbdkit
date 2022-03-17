#!/usr/bin/env bash
# nbdkit
# Copyright (C) 2018-2020 Red Hat Inc.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
# * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution.
#
# * Neither the name of Red Hat nor the names of its contributors may be
# used to endorse or promote products derived from this software without
# specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY RED HAT AND CONTRIBUTORS ''AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
# PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RED HAT OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
# USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
# OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.

source ./functions.sh
set -e
set -x

requires test -f disk
requires test -r /dev/null
requires nbdinfo --version

# Write a dummy cookiefile/jar.
cookiejar="cookiejar"
rm -f $cookiejar
cleanup_fn rm -f $cookiejar

cat > $cookiejar <<EOF
# Netscape HTTP Cookie File
# https://curl.haxx.se/docs/http-cookies.html
# This file was generated by libcurl! Edit at your own risk.

.libguestfs.org	TRUE	/	FALSE	2145916800	foo bar
EOF

# Although curl won't use these settings because we're using the file:
# protocol, this still exercises the paths inside the plugin.

for opt in \
    cainfo=/dev/null \
    capath=/dev/null \
    cookie=foo=bar \
    cookiefile= \
    cookiefile=$cookiejar \
    cookiejar=$cookiejar \
    followlocation=false \
    header="X-My-Name: John Doe" \
    header="User-Agent:" \
    header="X-Empty;" \
    password=secret \
    protocols=file,http,https \
    proxy-password=secret \
    proxy-user=eve \
    sslverify=false \
    tcp-keepalive=true \
    tcp-nodelay=false \
    timeout=120 \
    timeout=0 \
    user=alice \
    user-agent="Mozilla/1"
do
    nbdkit -fv -D curl.verbose=1 -U - \
           curl file:$PWD/disk protocols=file "$opt" \
           --run 'nbdinfo $nbd'
done
