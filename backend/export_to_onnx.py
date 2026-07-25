import os
import json
import torch
import torch.nn as nn
import torchvision.models as models

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LABELS_PATH = os.path.join(BASE_DIR, "labels.json")
MODEL_PATH = os.path.join(BASE_DIR, "tomato_disease_detector.pt")
ONNX_PATH = os.path.join(BASE_DIR, "tomato_disease_detector.onnx")

with open(LABELS_PATH, "r") as f:
    LABELS = json.load(f)

num_classes = len(LABELS)

def build_model(num_classes=10):
    model = models.mobilenet_v2(weights=None)
    in_features = model.classifier[1].in_features
    model.classifier = nn.Sequential(
        nn.Dropout(0.2),
        nn.Linear(in_features, num_classes)
    )
    return model

model = build_model(num_classes=num_classes)
if os.path.exists(MODEL_PATH):
    model.load_state_dict(torch.load(MODEL_PATH, map_location='cpu'))
    print(f"Loaded weights from {MODEL_PATH}")
else:
    print(f"Warning: {MODEL_PATH} not found")

model.eval()

# Dummy input to trace the model (batch_size=1, channels=3, 128x128)
dummy_input = torch.randn(1, 3, 128, 128)

torch.onnx.export(
    model,
    dummy_input,
    ONNX_PATH,
    export_params=True,
    opset_version=18,
    do_constant_folding=True,
    input_names=['input'],
    output_names=['output'],
    dynamic_axes={'input': {0: 'batch_size'}, 'output': {0: 'batch_size'}},
    dynamo=False
)

print(f"Exported ONNX model successfully to {ONNX_PATH}")
