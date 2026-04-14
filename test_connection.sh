r#!/bin/bash
echo "Testing Control Server Connection"
echo "================================="

# Check if editor exists
if [ ! -f "editor" ]; then
    echo "✗ Editor binary not found!"
    exit 1
fi

# Check if HTML file exists
if [ ! -f "control_server_webpage.html" ]; then
    echo "✗ HTML webpage file not found!"
    exit 1
fi

echo "✓ Editor binary exists"
echo "✓ HTML webpage file exists"

echo ""
echo "To test the connection:"
echo "1. Start the editor in one terminal: ./editor"
echo "2. Look for this line in the output: 'ControlServer listening on http://127.0.0.1:PORT'"
echo "3. Open a web browser to: http://127.0.0.1:PORT/"
echo ""
echo "The dashboard should show:"
echo "- Connection status: Connected (green indicator)"
echo "- Real-time stats updating every 5 seconds"
echo "- Individual clip statistics with search/filter"
echo "- Performance metrics"
echo "- Cache statistics"
echo ""
echo "If you see 'Connecting...' stuck:"
echo "1. Make sure the editor is running"
echo "2. Check that the ControlServer started (look for the listening message)"
echo "3. Try refreshing the page"
echo "4. Check browser console for errors (F12 → Console)"
echo ""
echo "The JavaScript has been improved to:"
echo "- Detect file:// protocol and show helpful error"
echo "- Try common ports if default fails"
echo "- Update URL to discovered port"
echo "- Show clear error messages"
echo ""
echo "Test complete!"