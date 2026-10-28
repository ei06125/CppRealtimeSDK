# Isolated single-CPU validation image containing RTX, ptests and automated SSH access.
# The strict runtime suite requires FIFO execution to pass rather than accepting a permission skip.
SUMMARY = "Development image for RTX kernel and communication validation"
require recipes-core/images/core-image-minimal.bb
inherit features_check testimage
REQUIRED_DISTRO_FEATURES = "ptest"

# This image is for isolated testing: root SSH access is needed by testimage.
IMAGE_FEATURES += "allow-empty-password empty-root-password allow-root-login ssh-server-dropbear"
IMAGE_INSTALL:append = " rtx rtx-ptest ptest-runner"
QB_SMP = "-smp 1"
QB_MEM = "-m 1024"
TEST_SUITES = "ping ssh rtx"
