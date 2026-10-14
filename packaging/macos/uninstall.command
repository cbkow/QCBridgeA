#!/bin/bash
# Remove QCBridgeA and its registration with QCView. Double-click in Finder.
echo "This removes /Applications/QCBridgeA.app and its QCView registration."
pkill -x QCBridgeA 2>/dev/null || true
sudo rm -rf "/Applications/QCBridgeA.app"
sudo rm -f "/Library/Application Support/QCView/Outputs/aja.json"
sudo pkgutil --forget com.qcview-app.QCBridgeA 2>/dev/null || true
echo "Done. QCView's settings for the output are left as they were."
