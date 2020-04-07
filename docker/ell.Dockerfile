#--------------------------------------------------------------------------
# Quickstart docker container for ELL (https://microsoft.github.io/ELL/)
# Ubuntu 18.04, Miniconda, Python 3.6
#--------------------------------------------------------------------------

FROM ell-dependencies

RUN conda create -n py36 numpy python=3.6
RUN activate py36

ENV ELL_ROOT "/ELL"

# Get the ELL source
ADD . ${ELL_ROOT}

WORKDIR /${ELL_ROOT}/build
RUN cmake ..
RUN make
RUN make _ELL_python

# Install ONNX for import-onnx.py
RUN pip install onnx

ENTRYPOINT ["/bin/bash"]