#--------------------------------------------------------------------------
# Quickstart docker container for ELL (https://microsoft.github.io/ELL/)
# Ubuntu 18.04, Miniconda, Python 3.6
#--------------------------------------------------------------------------

FROM ell

RUN pip install \
    argparse \
    validators

WORKDIR /model
ADD run-ell.py /model

ENTRYPOINT ["python", "/model/run-ell.py"]