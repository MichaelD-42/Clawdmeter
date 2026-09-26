# Reads dbus-monitor's PropertiesChanged signals for the board's REQ
# characteristic (see start_notify_subscriber in claude-usage-daemon.sh).
#   01        -> send data again: touch the refresh flag
#   02 lo hi  -> Allow on the board for permission request <hi><lo>
#   03 lo hi  -> Deny
# Variables: flag = refresh flag file, answer = command that takes "ID A".

/"Value"/ { grab = 1; n = 0; next }

grab {
    for (i = 1; i <= NF; i++)
        if ($i ~ /^[0-9a-f][0-9a-f]$/) b[++n] = $i
    if ($0 !~ /\]/) next
    grab = 0
    if (n == 1 && b[1] == "01") {
        system("touch '" flag "'")
    } else if (n == 3 && (b[1] == "02" || b[1] == "03")) {
        system(answer " " b[3] b[2] " " (b[1] == "02" ? "allow" : "deny"))
    }
    fflush()
}
