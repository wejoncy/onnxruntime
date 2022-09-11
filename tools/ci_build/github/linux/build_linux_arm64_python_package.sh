#/bin/bash
set -x
mkdir /build/dist
export CFLAGS="-Wp,-D_FORTIFY_SOURCE=2 -Wp,-D_GLIBCXX_ASSERTIONS -fstack-protector-strong -fstack-clash-protection -fcf-protection -O3 -Wl,--strip-all" 
export CXXFLAGS="-Wp,-D_FORTIFY_SOURCE=2 -Wp,-D_GLIBCXX_ASSERTIONS -fstack-protector-strong -fstack-clash-protection -fcf-protection -O3 -Wl,--strip-all"
PYTHON_EXES=("/opt/python/cp37-cp37m/bin/python3.7" "/opt/python/cp38-cp38/bin/python3.8" "/opt/python/cp39-cp39/bin/python3.9" "/opt/python/cp310-cp310/bin/python3.10")
for PYTHON_EXE in "${PYTHON_EXES[@]}"
do
  rm -rf /build/Release
  ${PYTHON_EXE} /onnxruntime_src/tools/ci_build/build.py \
                  --build_dir /build \
                  --config Release --update --build \
                  --skip_submodule_sync \
                  --parallel \
                  --enable_lto \
                  --build_wheel \
                  --enable_onnx_tests
  cp /build/Release/dist/*.whl /build/dist
done
