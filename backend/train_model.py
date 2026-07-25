import os
import json
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader
import torchvision.transforms as transforms
import torchvision.datasets as datasets
import torchvision.models as models

device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

# Standard ImageNet Mean & Std for MobileNetV2
MEAN = [0.485, 0.456, 0.406]
STD = [0.229, 0.224, 0.225]

def get_dataset_dir():
    candidates = [
        r"C:\Users\annam\Documents\model dataset\archive\tomato",
        r"C:\Users\annam\Downloads\archive\tomato",
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "dataset"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dataset")
    ]
    for candidate in candidates:
        if os.path.exists(candidate) and os.path.exists(os.path.join(candidate, 'train')):
            return candidate
    return None

def load_and_prep_data(data_dir):
    print(f"Loading dataset from: {data_dir}")
    
    train_transform = transforms.Compose([
        transforms.Resize((128, 128)),
        transforms.RandomHorizontalFlip(),
        transforms.RandomVerticalFlip(),
        transforms.RandomRotation(15),
        transforms.ColorJitter(brightness=0.1, contrast=0.1),
        transforms.ToTensor(),
        transforms.Normalize(mean=MEAN, std=STD)
    ])
    
    val_transform = transforms.Compose([
        transforms.Resize((128, 128)),
        transforms.ToTensor(),
        transforms.Normalize(mean=MEAN, std=STD)
    ])
    
    train_dir = os.path.join(data_dir, 'train')
    val_dir = os.path.join(data_dir, 'val')
    
    train_ds = datasets.ImageFolder(train_dir, transform=train_transform)
    val_ds = datasets.ImageFolder(val_dir, transform=val_transform)

    print(f"Loaded {len(train_ds)} training images and {len(val_ds)} validation images across {len(train_ds.classes)} classes.")

    train_loader = DataLoader(train_ds, batch_size=64, shuffle=True, num_workers=0)
    val_loader = DataLoader(val_ds, batch_size=64, shuffle=False, num_workers=0)
    
    # Save labels mapping for backend & server
    base_dir = os.path.dirname(os.path.abspath(__file__))
    labels_dict = {str(v): k for k, v in train_ds.class_to_idx.items()}
    
    labels_path = os.path.join(base_dir, "labels.json")
    with open(labels_path, "w") as f:
        json.dump(labels_dict, f, indent=4)
        
    print(f"Labels mapping saved to {labels_path}")
        
    return train_loader, val_loader, len(train_ds.classes), labels_dict

def build_model(num_classes=10):
    model = models.mobilenet_v2(weights=models.MobileNet_V2_Weights.DEFAULT)
    in_features = model.classifier[1].in_features
    model.classifier = nn.Sequential(
        nn.Dropout(0.2),
        nn.Linear(in_features, num_classes)
    )
    return model

def evaluate(model, val_loader, criterion):
    model.eval()
    val_loss = 0.0
    correct = 0
    total = 0
    with torch.no_grad():
        for inputs, labels in val_loader:
            inputs, labels = inputs.to(device), labels.to(device)
            outputs = model(inputs)
            loss = criterion(outputs, labels)
            val_loss += loss.item() * inputs.size(0)
            preds = outputs.argmax(dim=1)
            correct += (preds == labels).sum().item()
            total += labels.size(0)
    acc = correct / total if total > 0 else 0
    avg_loss = val_loss / total if total > 0 else 0
    return avg_loss, acc

def export_onnx(model, num_classes, onnx_path):
    model.eval()
    dummy_input = torch.randn(1, 3, 128, 128).to(device)
    torch.onnx.export(
        model,
        dummy_input,
        onnx_path,
        export_params=True,
        opset_version=14,
        do_constant_folding=True,
        input_names=['input'],
        output_names=['output'],
        dynamic_axes={'input': {0: 'batch_size'}, 'output': {0: 'batch_size'}}
    )
    print(f"Exported ONNX model successfully to {onnx_path}")

def main():
    print("=== Tomato Disease MobileNetV2 Training Pipeline ===")
    data_dir = get_dataset_dir()
    if not data_dir:
        print("Error: Could not locate dataset directory!")
        return

    train_loader, val_loader, num_classes, labels_dict = load_and_prep_data(data_dir)
    model = build_model(num_classes=num_classes).to(device)

    criterion = nn.CrossEntropyLoss()
    optimizer = optim.AdamW(model.parameters(), lr=1e-3, weight_decay=1e-4)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=4)

    best_acc = 0.0
    epochs = 4
    base_dir = os.path.dirname(os.path.abspath(__file__))
    pt_path = os.path.join(base_dir, "tomato_disease_detector.pt")
    onnx_path = os.path.join(base_dir, "tomato_disease_detector.onnx")

    print(f"Starting fine-tuning on {device} for {epochs} epochs...")

    for epoch in range(epochs):
        model.train()
        running_loss = 0.0
        correct = 0
        total = 0
        
        for i, (inputs, labels) in enumerate(train_loader):
            inputs, labels = inputs.to(device), labels.to(device)
            optimizer.zero_grad()
            outputs = model(inputs)
            loss = criterion(outputs, labels)
            loss.backward()
            optimizer.step()
            
            running_loss += loss.item() * inputs.size(0)
            preds = outputs.argmax(dim=1)
            correct += (preds == labels).sum().item()
            total += labels.size(0)
            
            if (i + 1) % 20 == 0 or (i + 1) == len(train_loader):
                print(f"Epoch [{epoch+1}/{epochs}] Step [{i+1}/{len(train_loader)}] Loss: {loss.item():.4f}")

        scheduler.step()
        train_acc = correct / total
        val_loss, val_acc = evaluate(model, val_loader, criterion)

        print(f"--> Epoch [{epoch+1}/{epochs}] Train Acc: {train_acc*100:.2f}% | Val Loss: {val_loss:.4f} | Val Acc: {val_acc*100:.2f}%")

        if val_acc > best_acc:
            best_acc = val_acc
            torch.save(model.state_dict(), pt_path)
            print(f"    New best validation accuracy: {best_acc*100:.2f}%! Saved weights to {pt_path}")

    print(f"\nTraining Complete. Best Validation Accuracy: {best_acc*100:.2f}%")
    
    # Load best model weights and export to ONNX
    model.load_state_dict(torch.load(pt_path, map_location=device))
    export_onnx(model, num_classes, onnx_path)

if __name__ == "__main__":
    main()
