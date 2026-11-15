#!/bin/bash
# Installs SAM 3D Body (Meta, 2026: full-body 3D pose from images, MHR body rig) in WSL Ubuntu for the mocap game.
# Run from Windows:  wsl -d Ubuntu -- bash /mnt/c/<repo>/tools/sam3d/install.sh
# Safe to run again. Skips SAM 3D Body's optional Detectron2 detector and MoGe field-of-view model: the person is
# found with YOLO (from GVHMR's install, or downloaded here) and the iPhone's lens is known.
# PyTorch 2.10 from conda-forge, the build that runs on RTX 50 series cards.
set -e
source ~/miniforge3/etc/profile.d/conda.sh 2>/dev/null || {
  wget -q https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh -O /tmp/miniforge.sh
  bash /tmp/miniforge.sh -b -p ~/miniforge3
  source ~/miniforge3/etc/profile.d/conda.sh
}
[ -d ~/sam-3d-body ] || git clone https://github.com/facebookresearch/sam-3d-body ~/sam-3d-body
cd ~/sam-3d-body

if ! conda env list | grep -q "^sam3d "; then
  echo "== Python environment"
  conda create -y -n sam3d python=3.11
fi
conda activate sam3d

if ! python -c "import torch, torchvision" 2>/dev/null; then
  echo "== PyTorch 2.10 (large download)"
  conda install -y 'pytorch==2.10.0' 'torchvision==0.25.0' 'ultralytics' -c conda-forge
fi
if ! python -c "import pytorch_lightning, roma, timm, huggingface_hub, smplx" 2>/dev/null; then
  echo "== Python packages"
  pip install pytorch-lightning opencv-python-headless yacs scikit-image einops timm dill pandas rich hydra-core \
    hydra-colorlog pyrootutils webdataset networkx==3.2.1 roma joblib appdirs cython jsonlines xtcocotools loguru \
    optree fvcore pycocotools huggingface_hub scipy smplx
fi

echo "== GPU check"
python -c "import torch; x = torch.randn(512, 512, device='cuda'); print(torch.__version__, torch.cuda.get_device_name(0), 'ok' if (x @ x).isfinite().all() else 'BAD')"

mkdir -p checkpoints
if [ ! -f checkpoints/yolov8x.pt ]; then
  wget -q https://github.com/ultralytics/assets/releases/download/v8.3.0/yolov8x.pt -O checkpoints/yolov8x.pt
fi
echo "== done"
