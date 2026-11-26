# Environment for the iMac Display live system.
export DISABLE_AUTOUPDATER=1
if [ -d /persist/home/.claude ]; then
  export CLAUDE_CONFIG_DIR=/persist/home/.claude
fi
