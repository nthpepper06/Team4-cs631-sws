#!/bin/sh
#
# Automated test suite for sws (Simple Web Server).
# CS631 Advanced Programming in the UNIX Environment
#

set -e

PORT=18080
SWS="./sws"
TESTDIR="/tmp/sws_test_$$"
DOCROOT="$TESTDIR/docroot"
CGIDIR="$TESTDIR/cgidir"
LOGFILE="$TESTDIR/sws.log"

echo "=== Setting up test environment in $TESTDIR ==="
mkdir -p "$DOCROOT" "$CGIDIR" "$DOCROOT/subdir"

# Create sample static files
echo "Hello CS631" > "$DOCROOT/hello.txt"
echo "<html><body><h1>Index Page</h1></body></html>" > "$DOCROOT/index.html"
echo "Subdir content" > "$DOCROOT/subdir/subfile.txt"
echo "Hidden file" > "$DOCROOT/subdir/.hidden"
echo "Visible file A" > "$DOCROOT/subdir/alpha.txt"
echo "Visible file Z" > "$DOCROOT/subdir/zeta.txt"

# Create sample CGI script
cat << 'EOF' > "$CGIDIR/test.cgi"
#!/bin/sh
echo "Content-Type: text/plain"
echo ""
echo "CGI_SUCCESS"
echo "REQUEST_METHOD=$REQUEST_METHOD"
echo "QUERY_STRING=$QUERY_STRING"
echo "SERVER_PROTOCOL=$SERVER_PROTOCOL"
EOF
chmod 755 "$CGIDIR/test.cgi"

# Create CGI script with custom status
cat << 'EOF' > "$CGIDIR/status.cgi"
#!/bin/sh
echo "Status: 202 Accepted"
echo "Content-Type: text/plain"
echo ""
echo "ACCEPTED_BODY"
EOF
chmod 755 "$CGIDIR/status.cgi"

# Create non-executable CGI script
echo "#!/bin/sh" > "$CGIDIR/noexec.cgi"
chmod 644 "$CGIDIR/noexec.cgi"

cleanup() {
	echo "=== Cleaning up ==="
	if [ -n "$SERVER_PID" ]; then
		kill "$SERVER_PID" 2>/dev/null || true
		wait "$SERVER_PID" 2>/dev/null || true
	fi
	rm -rf "$TESTDIR"
}
trap cleanup EXIT INT TERM

echo "=== Test 1: Usage and argument parsing ==="
$SWS -h >/dev/null 2>&1 || true
if $SWS >/dev/null 2>&1; then
	echo "FAIL: sws without arguments should fail"
	exit 1
fi
echo "PASS: usage and argument checks"

echo "=== Test 2: Starting sws in background ==="
$SWS -p $PORT -c "$CGIDIR" -l "$LOGFILE" "$DOCROOT"
sleep 1
SERVER_PID=$(pgrep -f "sws -p $PORT" | head -n 1 || true)
echo "PASS: sws daemon started (PID: $SERVER_PID)"

echo "=== Test 3: Basic GET static file ==="
RES=$(curl -s -i "http://127.0.0.1:$PORT/hello.txt")
echo "$RES" | grep "HTTP/1.0 200 OK" >/dev/null
echo "$RES" | grep "Hello CS631" >/dev/null
echo "PASS: GET /hello.txt returned 200 OK"

echo "=== Test 4: Default index.html resolution ==="
RES=$(curl -s -i "http://127.0.0.1:$PORT/")
echo "$RES" | grep "HTTP/1.0 200 OK" >/dev/null
echo "$RES" | grep "Index Page" >/dev/null
echo "PASS: GET / resolved to index.html"

echo "=== Test 5: Dynamic directory indexing ==="
rm -f "$DOCROOT/subdir/index.html"
RES=$(curl -s -i "http://127.0.0.1:$PORT/subdir/")
echo "$RES" | grep "HTTP/1.0 200 OK" >/dev/null
echo "$RES" | grep "alpha.txt" >/dev/null
echo "$RES" | grep "zeta.txt" >/dev/null
if echo "$RES" | grep "\.hidden" >/dev/null; then
	echo "FAIL: directory index included hidden file"
	exit 1
fi
echo "PASS: directory index generated and dotfiles ignored"

echo "=== Test 6: HEAD request (no body) ==="
BODY=$(curl -s -I "http://127.0.0.1:$PORT/hello.txt")
echo "$BODY" | grep "HTTP/1.0 200 OK" >/dev/null
echo "$BODY" | grep "Content-Length: 12" >/dev/null
echo "PASS: HEAD request succeeded"

echo "=== Test 7: If-Modified-Since (304 Not Modified) ==="
FUTURE_DATE="Sun, 01 Jan 2030 00:00:00 GMT"
RES=$(curl -s -i -H "If-Modified-Since: $FUTURE_DATE" "http://127.0.0.1:$PORT/hello.txt")
echo "$RES" | grep "HTTP/1.0 304 Not Modified" >/dev/null
echo "PASS: conditional GET returned 304 Not Modified"

echo "=== Test 8: Non-existent file (404 Not Found) ==="
RES=$(curl -s -i "http://127.0.0.1:$PORT/nonexistent.html")
echo "$RES" | grep "HTTP/1.0 404 Not Found" >/dev/null
echo "PASS: non-existent file returned 404"

echo "=== Test 9: Path traversal attack prevention ==="
RES=$(curl -s -i --path-as-is "http://127.0.0.1:$PORT/../../../etc/passwd")
echo "$RES" | grep -E "HTTP/1.0 (400|403|404)" >/dev/null
echo "PASS: directory traversal blocked"

echo "=== Test 10: Unsupported HTTP Method (501 Not Implemented) ==="
RES=$(curl -s -i -X POST "http://127.0.0.1:$PORT/hello.txt")
echo "$RES" | grep "HTTP/1.0 501 Not Implemented" >/dev/null
echo "PASS: POST method returned 501 Not Implemented"

echo "=== Test 11: CGI Execution ==="
RES=$(curl -s -i "http://127.0.0.1:$PORT/cgi-bin/test.cgi?foo=bar&num=42")
echo "$RES" | grep "HTTP/1.0 200 OK" >/dev/null
echo "$RES" | grep "CGI_SUCCESS" >/dev/null
echo "$RES" | grep "QUERY_STRING=foo=bar&num=42" >/dev/null
echo "PASS: CGI execution with query string succeeded"

echo "=== Test 12: CGI Custom Status ==="
RES=$(curl -s -i "http://127.0.0.1:$PORT/cgi-bin/status.cgi")
echo "$RES" | grep "HTTP/1.0 202 Accepted" >/dev/null
echo "$RES" | grep "ACCEPTED_BODY" >/dev/null
echo "PASS: CGI custom status handled"

echo "=== Test 13: CGI Permission Check (403 Forbidden) ==="
RES=$(curl -s -i "http://127.0.0.1:$PORT/cgi-bin/noexec.cgi")
echo "$RES" | grep "HTTP/1.0 403 Forbidden" >/dev/null
echo "PASS: non-executable CGI script returned 403 Forbidden"

echo "=== Test 14: Log file verification ==="
sleep 1
if [ ! -s "$LOGFILE" ]; then
	echo "FAIL: log file is empty"
	exit 1
fi
grep "hello.txt" "$LOGFILE" >/dev/null
grep "200" "$LOGFILE" >/dev/null
echo "PASS: log file contains expected entries"

echo ""
echo "================================"
echo "ALL TESTS PASSED SUCCESSFULLY!"
echo "================================"
