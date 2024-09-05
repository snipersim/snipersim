#!/usr/bin/env python3

import os
import sys
import subprocess

programs_to_find = [
{"name": "GNU C++ Compiler", "program": "g++"},
{"name": "Wget Downloader", "program": "wget"}
]  # Add programs you want to search for

headers_to_find = [
{"name": "zlib1g-dev / zlib-devel", "header_file": "zlib.h"},
{"name": "libbz2-dev / bzip2-devel", "header_file": "bzlib.h"},
{"name": "python3-dev / python3-devel", "header_file": "Python.h"},
{"name": "Boost", "header_file": "boost"},
{"name": "libsqlite3-dev / sqlite-devel", "header_file": "sqlite3.h"}
]  # Add header files and directories you want to search for

def get_gcc_include_paths():
    try:
        # Run gcc command to get include paths
        result = subprocess.run(['gcc', '-E', '-x', 'c', '-', '-v'], input='', stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        include_paths = []
        capture = False
        for line in result.stderr.splitlines():
            if line.strip() == '#include <...> search starts here:':
                capture = True
                continue
            if capture:
                if (line.strip() == 'End of search list.'):
                    break
                include_paths.append(line.strip())
        return include_paths
    except FileNotFoundError:
        print("GCC is not installed or not found in the system PATH.")
        return []

def find_environment_include_paths():
    extra_paths = []
    if os.environ.get('BOOST_INCLUDE', ''):
        extra_paths.append(os.environ['BOOST_INCLUDE'])
    if os.environ.get('SQLITE_PATH', ''):
        extra_paths.append(os.environ['SQLITE_PATH'])

    return extra_paths


def find_header_files(header_files, include_paths):
    found_files = {header['header_file']: None for header in header_files}
    for path in include_paths:
        for root, dirs, files in os.walk(path):
            for header in header_files:
                header_file = header['header_file']
                if header_file in files or header_file in dirs:
                    found_files[header_file] = os.path.join(root, header_file)
    return found_files

def find_programs(programs):
    found_programs = {program['program']: None for program in programs}
    for path in os.environ["PATH"].split(os.pathsep):
        for program in programs:
            program_name = program['program']
            program_path = os.path.join(path, program_name)
            if os.path.isfile(program_path) and os.access(program_path, os.X_OK):
                found_programs[program_name] = program_path
    return found_programs

if __name__ == "__main__":
    missing_dependency = False
    # Check programs first
    found_programs = find_programs(programs_to_find)

    for program in programs_to_find:
        program_name = program['program']
        if not found_programs[program_name]:
            print(f"Program '{program_name}' not found in the PATH. Please install '{program['name']}'.")
            missing_dependency = True

    # Check libraries
    include_paths = get_gcc_include_paths()
    include_paths += find_environment_include_paths();

    if not include_paths:
        print("No GCC include paths found.")
    else:
        found_files = find_header_files(headers_to_find, include_paths)

        for header in headers_to_find:
            header_file = header['header_file']
            if not found_files[header_file]:
                print(f"Header '{header_file}' not found in the GCC include paths. Please install '{header['name']}'.")
                missing_dependency = True

    if missing_dependency:
        sys.exit(1)
    else:
        sys.exit(0)
