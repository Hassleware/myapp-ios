#!/bin/bash
# Installs GVHMR (world-grounded human motion from video) in WSL Ubuntu, for the mocap game's PC side.
# Run from Windows:  wsl -d Ubuntu -- bash /mnt/c/<repo>/tools/gvhmr/install.sh
# Safe to run again: every step skips what is already done.
# The recipe for RTX 50 series cards (Blackwell, sm_120) is from GVHMR issue #88 (May 2026):
# the repo's own requirements.txt pins PyTorch 2.3, which can't run on these cards.
set -e
cd ~

if [ ! -d ~/miniforge3 ]; then
  echo "== Miniforge (conda)"
  wget -q https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh -O /tmp/miniforge.sh
  bash /tmp/miniforge.sh -b -p ~/miniforge3
fi
source ~/miniforge3/etc/profile.d/conda.sh

if [ ! -d ~/GVHMR ]; then
  echo "== GVHMR source"
  git clone https://github.com/zju3dv/GVHMR ~/GVHMR
fi
cd ~/GVHMR

if ! conda env list | grep -q "^gvhmr "; then
  echo "== Python environment"
  conda create -y -n gvhmr python=3.10
fi
conda activate gvhmr

if ! python -c "import pytorch3d" 2>/dev/null; then
  echo "== PyTorch 2.10, PyTorch3D and friends (large download)"
  conda install -y 'pytorch==2.10.0' 'torchvision==0.25.0' 'ultralytics==8.4.52' 'pytorch3d=0.7.9=cuda130py310*'     'ffmpeg-python==0.2.0' -c conda-forge
fi
if ! python -c "import hydra, smplx, timm, lightning, gdown, chumpy" 2>/dev/null; then
  echo "== Python packages"
  pip install numpy==1.23.5 timm==0.9.12 lightning==2.3.0 scikit-image==0.24.0 hydra-core==1.3 hydra-zen hydra-colorlog     rich einops av==13.0.0 joblib trimesh smplx wis3d pycolmap cython_bbox lapx gdown
  pip install chumpy --no-build-isolation   # old package: it can't build in pip's isolated build environment
fi
if ! (cd ~ && python -c "import hmr4d" 2>/dev/null); then   # from ~, so the source folder itself does not count
  pip install -e . --no-deps
fi

echo "== GPU check"
python -c "import torch; x = torch.randn(512, 512, device='cuda'); print(torch.__version__, torch.cuda.get_device_name(0), 'ok' if (x @ x).isfinite().all() else 'BAD')"

mkdir -p inputs/checkpoints outputs
if [ ! -f inputs/checkpoints/gvhmr/gvhmr_siga24_release.ckpt ]; then
  echo "== Model weights from GVHMR's Google Drive folder (several GB)"
  gdown --folder https://drive.google.com/drive/folders/1eebJ13FUEXrKBawHpJroW0sNSxLjh9xD -O inputs/checkpoints
fi
ls -R inputs/checkpoints | head -40

echo "== Body models (SMPL / SMPL-X need a free registration, done by hand)"
# Put the downloaded files in the repo's tools/.bin/body_models/smplx/ (and smpl/); git ignores tools/.bin.
SOURCE="$(dirname "$(readlink -f "$0")")/../.bin/body_models"
for kind in smplx smpl; do
  if ls "$SOURCE/$kind/"* >/dev/null 2>&1; then
    mkdir -p inputs/checkpoints/body_models/$kind
    cp -n "$SOURCE/$kind/"* inputs/checkpoints/body_models/$kind/
  fi
done
for f in inputs/checkpoints/body_models/smplx/SMPLX_NEUTRAL.npz inputs/checkpoints/body_models/smpl/SMPL_NEUTRAL.pkl; do
  [ -f "$f" ] && echo "found $f" || echo "MISSING $f"
done
echo "== done"
