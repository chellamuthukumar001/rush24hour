import os
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader
import torchvision.transforms as transforms
import torchvision.datasets as datasets
import json

device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

def load_and_prep_data(data_dir):
    print(f"Loading dataset from local folder: {data_dir}")
    
    transform = transforms.Compose([
        transforms.Resize((128, 128)),
        transforms.ToTensor(),
    ])
    
    train_dir = os.path.join(data_dir, 'train')
    val_dir = os.path.join(data_dir, 'val')
    
    train_ds = datasets.ImageFolder(train_dir, transform=transform)
    val_ds = datasets.ImageFolder(val_dir, transform=transform)

    print(f"Loaded {len(train_ds)} training images and {len(val_ds)} validation images.")

    train_loader = DataLoader(train_ds, batch_size=32, shuffle=True)
    val_loader = DataLoader(val_ds, batch_size=32)
    
    # Save labels mapping for the backend to use
    labels_dict = {v: k for k, v in train_ds.class_to_idx.items()}
    with open("labels.json", "w") as f:
        json.dump(labels_dict, f, indent=4)
        
    print(f"Labels mapping saved: {labels_dict}")
        
    return train_loader, val_loader, len(train_ds.classes)

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

def main():
    print("=== Tomato Disease Custom PyTorch Model Training Pipeline ===")
    data_dir = r"C:\Users\annam\Downloads\archive\tomato"
    
    if not os.path.exists(data_dir):
        print(f"Error: Dataset directory {data_dir} not found!")
        return
        
    train_loader, val_loader, num_classes = load_and_prep_data(data_dir)

    print(f"Detected {num_classes} classes from the dataset.")

    model = CustomCNN(num_classes=num_classes).to(device)
    criterion = nn.CrossEntropyLoss()
    optimizer = optim.Adam(model.parameters(), lr=0.001)

    print(f"Starting training on {device}...")
    epochs = 3 
    for epoch in range(epochs):
        model.train()
        running_loss = 0.0
        for i, (inputs, labels) in enumerate(train_loader):
            inputs, labels = inputs.to(device), labels.to(device)
            optimizer.zero_grad()
            outputs = model(inputs)
            loss = criterion(outputs, labels)
            loss.backward()
            optimizer.step()
            running_loss += loss.item()
            
            if (i+1) % 50 == 0:
                print(f"Epoch {epoch+1}/{epochs} - Batch {i+1}/{len(train_loader)} - Loss: {running_loss/(i+1):.4f}")
        
        print(f"Epoch {epoch+1}/{epochs} - Average Loss: {running_loss/len(train_loader):.4f}")

    save_path = "tomato_disease_detector.pt"
    torch.save(model.state_dict(), save_path)
    print(f"Model weights saved to {save_path}")
    print("Training complete! You can now run the web backend.")

if __name__ == "__main__":
    main()
