# Building Oldassity

Oldassity is developed and tested primarily on Ubuntu Linux.

## Dependencies

Install the system build dependencies:

```sh
sudo apt install build-essential cmake git pkg-config \
    libgtk2.0-dev libasound2-dev libjack-jackd2-dev \
    uuid-dev libmpg123-dev
```

Additional dependencies are managed by Conan 2. With `uv` installed,
create an isolated Python environment from the repository root:

```sh
uv venv --python 3.12 build/conan/env
uv pip install --python build/conan/env/bin/python 'conan>=2,<3'
```

## Build

From the repository root:

```sh
source build/conan/env/bin/activate
export CMAKE_POLICY_VERSION_MINIMUM=3.5

cmake -S . -B build -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -Daudacity_use_libmpg123=system

cmake --build build --parallel
```

## Install

```sh
sudo cmake --install build
```

Launch with:

```sh
oldassity
```

## Rebuild

For subsequent builds:

```sh
source build/conan/env/bin/activate
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build --parallel
sudo cmake --install build
```
