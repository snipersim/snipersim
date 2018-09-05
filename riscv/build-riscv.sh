ENV_SCR=$(readlink -f "${BASH_SOURCE}")
TOOLS_DIR=$(dirname $(dirname "${ENV_SCR}"))
LOCAL_ROOT="${TOOLS_DIR}/riscv"

if [ -z  "$SNIPER_ROOT" ]; then
	export SNIPER_ROOT=$TOOLS_DIR
	echo "Setting SNIPER_ROOT to $TOOLS_DIR"
fi

### 0) Check Dependencies
"${SNIPER_ROOT}/tools/checkdependencies.py"
if [ $? -ne 0 ]; then
	echo "Resolve dependencies and come back!"
	exit 1
fi
"${SNIPER_ROOT}/tools/checkdependencies-riscv.sh"
if [ $? -ne 0 ]; then
	echo "Resolve dependencies and come back!"
	exit 1
fi

if [ -z  "$PIN_ROOT" ]; then
   echo "  Please set the PIN_ROOT environment variable to your copy of Pin"
   exit 1
fi

if [ -z  "$CPU2006_ROOT" ]; then
   echo "  Please set the CPU2006_ROOT environment variable to your installed copy of SPEC CPU2006 v1.2"
   exit 1
fi

export RISCV=$LOCAL_ROOT/riscv-tools/RV64G
export PATH=$RISCV/bin:$PATH
export RV8_HOME=$LOCAL_ROOT/rv8
export SPECKLE_ROOT=$LOCAL_ROOT/Speckle
export SPEC_DIR=$CPU2006_ROOT

NPROC=(`nproc --all`)
updateGitRepo() {
	URL=$1
	BRANCH=$2
	FOLDER=$3
	cd $LOCAL_ROOT
	if [ ! -d $FOLDER ]; then
		git clone -b $BRANCH $URL $FOLDER
		cd $FOLDER
		git submodule update --init --recursive
	else
		cd $FOLDER
		git pull
		git submodule update --recursive
	fi
}

### 1) Setting up pre-requisites

# 1a) Sniper
echo "Setting up Pin for Sniper..."
cd $SNIPER_ROOT
[[ ! -L "pin_kit" && ! -d "pin_kit" ]] && ln -s $PIN_ROOT pin_kit
echo "####################################################################################"

# 1b) riscv-tools - includes Spike (that support sift generation)
echo "Setting up riscv-tools..."
URL=https://github.com/nus-comparch/riscv-tools.git
BRANCH=sift
FOLDER=riscv-tools
updateGitRepo "$URL" "$BRANCH" "$FOLDER"
echo "####################################################################################"

# 1c) rv8 simulator (that support sift generation)
echo "Setting up rv8 simulator..."
URL=https://github.com/nus-comparch/rv8.git
BRANCH=sift
FOLDER=rv8
updateGitRepo "$URL" "$BRANCH" "$FOLDER"
echo "####################################################################################"

# 1d) Speckle
echo "Setting up Speckle..."
URL=https://github.com/nus-comparch/Speckle.git
BRANCH=sift
FOLDER=Speckle
updateGitRepo "$URL" "$BRANCH" "$FOLDER"
echo "####################################################################################"

### 2) Compiling Binaries
# 2a) Sniper
echo "Compiling Sniper..."
cd $SNIPER_ROOT
make # TODO: Parallel builds currently broken
if [ $? -ne 0 ]; then
   echo "Compiling Sniper failed!"
   exit 1
fi
echo "####################################################################################"

# 2b) riscv-tools (includes Spike)
echo "Building riscv-tools..."
cd $LOCAL_ROOT/riscv-tools
echo "Building RISC-V Tools with $NPROC process(es)"
./build-sift.sh $NPROC
if [ $? -ne 0 ]; then
   echo "Building riscv-tools failed!"
   exit 1
fi
echo "####################################################################################"

# 2c) rv8
#echo "Compiling rv8 simulator..."
#cd $RV8_HOME
#make test-build TEST_RV64="ARCH=rv64imafd TARGET=riscv64-unknown-elf"
#make -j $NPROC
#echo "####################################################################################"

# 2d) Speckle - to compile and copy SPEC CPU2006 binaries
echo "Compiling SPEC CPU2006 binaries..."
cd $SPECKLE_ROOT
./gen_binaries_sift.sh --compile --copy
echo "####################################################################################"


# 3) Running SPEC binaries to generate SIFT traces
echo "Running SPEC binaries on Spike simulator to generate SIFT traces..."

# 3a) Spike
### Eg:3a-i) Using script
cd $SPECKLE_ROOT
# run_sift.sh assumes SPEC is already compiled and binaries copied to $SPECKLE_ROOT/riscv-spec-test
./run_sift.sh --benchmark 462.libquantum # running for a single benchmark
# ./run_sift.sh --all # running for all benchmarks
echo "####################################################################################"


### Eg:3a-ii) Without script
# running individual binaries in Spike
# cd $SPECKLE_ROOT/riscv-spec-test/456.hmmer
# spike --sift=hmmer-1.sift pk  hmmer --fixed 0 --mean 325 --num 45000 --sd 200 --seed 0 bombesin.hmm
# echo "####################################################################################"


# 3b) rv8
### Eg:3b-i) Using script
# cd $SPECKLE_ROOT
# Change SIMULATOR=rv8 in run_sift.sh#7
# run_sift.sh assumes SPEC is already compiled and binaries copied to $SPECKLE_ROOT/riscv-spec-test
# ./run_sift.sh --benchmark 462.libquantum # running for a single benchmark
# ./run_sift.sh --all # running for all benchmarks
# echo "####################################################################################"


### Eg:3b-ii) Without script
# running individual binaries in rv8
# cd $SPECKLE_ROOT/riscv-spec-test/462.libquantum
# $RV8_HOME/build/linux_x86_64/bin/rv-jit --log-sift --log-sift-filename libquantum-1.sift libquantum 33 5
# echo "####################################################################################"


# 4) Running SIFT traces with Sniper
echo "Running SIFT traces with Sniper..."
# Running the traces generated by Spike (assuming Eg:3a-i was already executed)
cd $SPECKLE_ROOT/output/spike/462.libquantum
$SNIPER_ROOT/run-sniper -criscv --traces=libquantum-1.sift
echo "####################################################################################"


# Running the traces generated by rv8 Simulator (assuming Eg:3b-ii was already executed)
# cd $SPECKLE_ROOT/output/rv8/462.libquantum
# $SNIPER_ROOT/run-sniper -criscv --traces=libquantum-1.sift
# echo "####################################################################################"

echo "export RISCV=$RISCV"
echo "export PATH=$PATH"
echo "export RV8_HOME=$RV8_HOME"
echo "export SNIPER_ROOT=$SNIPER_ROOT"
echo "export SPEC_DIR=$CPU2006_ROOT"
echo "export SPECKLE_ROOT=$SPECKLE_ROOT"

echo "####################################################################################"
