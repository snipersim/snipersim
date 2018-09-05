

checkDependencies () { # "${dependencies[@]}"
	local package
	local rc=0
	for package in "$@"; do
		dpkg-query -W "${package}" 2>> /dev/null >&2
		if (( $? > 0 )); then
			echo "Missing package $package"
			rc=1
		fi
	done
	return $rc
}


riscvtoolsdependencies=('autoconf' 'automake' 'autotools-dev' 'curl' 'libmpc-dev' 'libmpfr-dev' 'libgmp-dev' 'libusb-1.0-0-dev' 'gawk' 'git' 'build-essential' 'bison' 'flex' 'texinfo' 'gperf' 'libtool' 'patchutils' 'bc' 'zlib1g-dev' 'device-tree-compiler' 'pkg-config' 'libexpat-dev')
checkDependencies "${riscvtoolsdependencies[@]}"
num_missing=$?
if [ "$num_missing" -gt 0 ]; then
	exit 1
else
	exit 0
fi
