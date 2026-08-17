#!/bin/bash

export PATH=~/go/bin:$PATH

import_path=$GRPCURL_IMPORT_PATH
proto_file="schedinfo.proto"
grpc_server="localhost:50052"
grpc_method="schedinfo.v1.SchedInfoService.AddSchedInfo"
json_file=""

usage() {
	echo "Usage: $0 [-i import_path] [-p protobuf_file] [-s grpc_server] [-m grpc_method] [-h] json_file" >&2
	echo "    import_path is set with 'GRPCURL_IMPORT_PATH' env variable by default:" >&2
	echo "    $ export GRPCURL_IMPORT_PATH=/dirpath/to/protobuf" >&2
}

while getopts "i:p:s:m:h" opt; do
	case $opt in
	i)
		import_path="$OPTARG"
		;;
	p)
		proto_file="$OPTARG"
		;;
	s)
		grpc_server="$OPTARG"
		;;
	m)
		grpc_method="$OPTARG"
		;;
	h)
		usage
		exit 0
		;;
	\?)
		usage
		exit 1
		;;
	esac
done

# shift past all processed options and arguments
shift $((OPTIND - 1))

# check for json_file argument
if [ -z "$1" ]; then
	echo "Error: Missing required argument 'json_file'" >&2
	usage
	exit 1
fi

json_file="$1"

if [ ! -d "$import_path" ]; then
	echo "Error: Import path '$import_path' does not exist or is not a directory" >&2
	usage
	exit 1
fi

if ! command -v grpcurl &> /dev/null; then
	echo "Error: grpcurl is not installed or not in PATH" >&2
	echo "Installing grpcurl with the following command:" >&2
	set -x
	sudo apt install -y golang
	go install github.com/fullstorydev/grpcurl/cmd/grpcurl@latest
	set +x
	exit 0
fi

echo "Starting grpcurl with the following parameters:"
echo "  import_path: $import_path"
echo "  proto_file: $proto_file"
echo "  grpc_server: $grpc_server"
echo "  grpc_method: $grpc_method"

grpcurl -v -plaintext \
	-import-path $import_path \
	-proto $proto_file \
	-d @ \
	$grpc_server \
	$grpc_method \
	< $json_file
if [ $? -ne 0 ]; then
	echo "Error: grpcurl command failed" >&2
	exit 1
fi
echo -e "\n\nDone. grpcurl command completed successfully"
exit 0
