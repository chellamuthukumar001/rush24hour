import torch
import torch.nn as nn
import os
import json

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LABELS_PATH = os.path.join(BASE_DIR, "labels.json")
MODEL_PATH = os.path.join(BASE_DIR, "tomato_disease_detector.pt")

with open(LABELS_PATH, "r") as f:
    LABELS = json.load(f)

class CustomCNN(nn.Module):
    def __init__(self, num_classes=10):
        super(CustomCNN, self).__init__()
        self.conv1 = nn.Conv2d(3, 16, 3, padding=1)
        self.relu = nn.ReLU()
        self.pool = nn.MaxPool2d(2, 2)
        self.conv2 = nn.Conv2d(16, 32, 3, padding=1)
        self.fc1 = nn.Linear(32 * 32 * 32, 128)
        self.fc2 = nn.Linear(128, num_classes)

    def forward(self, x):
        x = self.pool(self.relu(self.conv1(x)))
        x = self.pool(self.relu(self.conv2(x)))
        x = x.view(-1, 32 * 32 * 32)
        x = self.relu(self.fc1(x))
        x = self.fc2(x)
        return x

model = CustomCNN(num_classes=len(LABELS) or 10)
model.load_state_dict(torch.load(MODEL_PATH, map_location='cpu'))
model.eval()

# Dummy input to trace the model (batch_size=1, channels=3, 128x128)
dummy_input = torch.randn(1, 3, 128, 128)

onnx_path = os.path.join(BASE_DIR, "tomato_disease_detector.onnx")
torch.onnx.export(model, dummy_input, onnx_path, 
                  export_params=True,
                  opset_version=14,
                  do_constant_folding=True,
                  input_names=['input'], output_names=['output'],
                  dynamic_axes={'input': {0: 'batch_size'}, 'output': {0: 'batch_size'}})
print(f"Exported ONNX model to {onnx_path}")
