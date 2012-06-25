#
# Regular cron jobs for the nix package
#
0 4	* * *	root	[ -x /usr/bin/nix-channel ] && /usr/bin/nix-channel --update
