########################
# BUILDING DOCKER IMAGES
########################

# The "ell-dependencies" Dockerfile contains the pre-reqs needed to build ELL, but does not include ELL itself.
# This should only need to be generated once, as the base dependencies rarely change.
# Note: this is used as the base ("FROM") image for the "ell-base" Dockerfile.
docker build -t ell-dependencies -f ell-dependencies.Dockerfile .

# The "ell-base" Dockerfile builds ELL and all included tools. This can serve as a continuous integration and
# testing image.
# Note: this depends on the presence of a "ell-dependencies" image, as generated above.
# Note: to pass in the ELL source during docker build, the context is set to the parent directory ("..")
# Note: we recommend building with the '--no-cache' flag, to ensure a clean build
docker build --no-cache -t ell-base -f ell-base.Dockerfile ..

# The "ell-run" Dockerfile adds an entrypoint script that calls ELL's "import-onnx.py" and "wrap.py" tools.
# See calling instructions below.
# Note: this depends on the presence of a local "ell-base" image, as generated above.
# Note: we recommend building with the '--no-cache' flag, to ensure a clean build
docker build --no-cache -t ell-run -f ell-run.Dockerfile .



########################
# RUNNING DOCKER IMAGES
########################

# The most convenient way to run the ELL toolchain will be with the "ell-run" image. This image has
# an entrypoint that calls the "wrap.py" script, and should be sufficient for most purposes.

# STEP 1: Create a docker container, and specify the targets as a comma-separated list of args
# (example shown for Raspberry Pi Zero and Pi 3)
docker create --name ell-run-task ell-run --target pi0,pi3

# STEP 2: Copy your onnx model over to the container (Assumes you have a local "model.onnx" file)
docker cp model.onnx ell-run-task:/model/model.onnx

# STEP 3: Run the ELL script
docker start -i ell-run-task

# STEP 4: Extract the artifacts. This will copy the entire "/model" directory from the container into
a model directory under the local path (".").
docker cp ell-run-task:/model .

# STEP 5: Remove the container
docker rm -fv ell-run-task



###################################
# RUNNING DOCKER IMAGES - ADVANCED
###################################

# If you want to run the ELL commands yourself, you may boot up an ell-base container
# with the command:
docker run --rm -it ell-base
