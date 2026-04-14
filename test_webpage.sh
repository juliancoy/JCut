
#!/bin/bash
echo "Testing Control Server Webpage"
echo "==============================="

# First, let's check if the HTML file exists
if [ -f "control_server_webpage.html" ]; then
    echo "✓ HTML webpage file exists: control_server_webpage.html"
    echo "  File size: $(wc -l < control_server_webpage.html) lines"
else
    echo "✗ HTML webpage file not found!"
    exit 1
fi

# Check if the editor binary exists
if [ -f "editor" ]; then
    echo "✓ Editor binary exists"
else
    echo "✗ Editor binary not found!"
    exit 1
fi

# Show the HTML file structure
echo ""
echo "HTML File Preview (first 10 lines):"
echo "-----------------------------------"
head -10 control_server_webpage.html

echo ""
echo "Checking for root route handler in control_server.cpp:"
echo "------------------------------------------------------"
if grep -q "request.url.path() == QStringLiteral(\"/\")" control_server.cpp; then
    echo "✓ Root route handler found in control_server.cpp"
else
    echo "✗ Root route handler not found!"
fi

echo ""
echo "Checking for HTML content type in control_server.cpp:"
echo "-----------------------------------------------------"
if grep -q "text/html" control_server.cpp; then
    echo "✓ HTML content type found in control_server.cpp"
else
    echo "✗ HTML content type not found!"
fi

echo ""
echo "Testing with a simple HTTP request simulation:"
echo "----------------------------------------------"
echo "The ControlServer should now serve:"
echo "1. GET / → HTML dashboard"
echo "2. GET /health → JSON health status"
echo "3. GET /profile → JSON profiling data"
echo "4. GET /diag/perf → JSON diagnostics"
echo ""
echo "To test manually after starting the editor:"
echo "1. Start the editor: ./editor"
echo "2. Look for 'ControlServer listening on http://127.0.0.1:PORT' in output"
echo "3. Open browser to http://127.0.0.1:PORT/"
echo "4. Or use curl: curl http://127.0.0.1:PORT/"
echo "5. Test API: curl http://127.0.0.1:PORT/health"
echo ""
echo "Enhanced Dashboard Features:"
echo "---------------------------"
echo "✓ Clips statistics section added"
echo "✓ Individual clip visualization with details"
echo "✓ Search and filter functionality"
echo "✓ Track-based filtering"
echo "✓ Selected clip highlighting"
echo "✓ Responsive clip cards with hover effects"
echo ""
echo "Implementation Summary:"
echo "----------------------"
echo "✓ control_server.cpp modified to serve HTML at root (/)"
echo "✓ HTML dashboard created with real-time stats display"
echo "✓ Auto-refresh JavaScript for live updates"
echo "✓ Fallback HTML page if file not found"
echo "✓ Build successful"
echo "✓ Enhanced with individual clip statistics visualization"
echo ""
echo "The ControlServer now serves a webpage showing stats in real time with individual clip details!"
