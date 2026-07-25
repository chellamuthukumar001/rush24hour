import os
import json
import io
import cv2
from PIL import Image
try:
    import numpy as np
except ImportError:
    np = None
try:
    import onnxruntime as ort
except ImportError:
    ort = None

from fastapi import FastAPI, File, UploadFile
from fastapi.middleware.cors import CORSMiddleware

app = FastAPI(title="TomAItrix Neural Farming API")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Use absolute paths for models and labels
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LABELS_PATH = os.path.join(BASE_DIR, "labels.json")
MODEL_PATH = os.path.join(BASE_DIR, "tomato_disease_detector.onnx")

LABELS = {}
if os.path.exists(LABELS_PATH):
    with open(LABELS_PATH, "r") as f:
        LABELS = json.load(f)

print("Loading local ONNX model...")
model_loaded = False
ort_session = None

def load_model():
    global ort_session, model_loaded
    try:
        if os.path.exists(MODEL_PATH) and ort is not None:
            ort_session = ort.InferenceSession(MODEL_PATH)
            model_loaded = True
            print("ONNX Model loaded successfully!")
        else:
            print(f"Model file not found at {MODEL_PATH} or onnxruntime missing")
    except Exception as e:
        print(f"Error loading ONNX model: {e}")

load_model()

# ImageNet normalization for MobileNetV2
MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32).reshape(1, 3, 1, 1) if np else None
STD = np.array([0.229, 0.224, 0.225], dtype=np.float32).reshape(1, 3, 1, 1) if np else None

def preprocess(image: Image.Image) -> np.ndarray:
    image = image.resize((128, 128), Image.Resampling.BILINEAR).convert("RGB")
    arr = np.array(image, dtype=np.float32) / 255.0
    arr = np.transpose(arr, (2, 0, 1))
    arr = np.expand_dims(arr, axis=0)
    arr = (arr - MEAN) / STD
    return arr.astype(np.float32)

def is_genuine_tomato_leaf(pil_img: Image.Image):
    """
    Multi-Feature Botanical Leaf Image Validator.
    Rejects non-leaf photos (people, faces, animals, buildings, cars, flat backgrounds, rooms, clothes).
    """
    if np is None:
        return True, "OK"
    
    img = pil_img.convert("RGB")
    cv_img = np.array(img)
    arr = cv_img.astype(np.float32)
    r, g, b = arr[:, :, 0], arr[:, :, 1], arr[:, :, 2]
    
    # 1. Laplacian Edge Texture Complexity Check (flat skin, walls, furniture lack leaf vein texture)
    gray = cv2.cvtColor(cv_img, cv2.COLOR_RGB2GRAY)
    lap_var = float(cv2.Laplacian(gray, cv2.CV_64F).var())
    if lap_var < 15.0:
        return False, f"Image lacks leaf texture or vein contours (Laplacian var: {lap_var:.1f})."

    hsv = cv2.cvtColor(cv_img, cv2.COLOR_RGB2HSV)
    h, s, v = hsv[:, :, 0], hsv[:, :, 1], hsv[:, :, 2]

    # 2. Non-Leaf / Human Skin Tone Object Detection
    skin_mask = (r > 90) & (g > 40) & (b > 20) & (r > g + 18) & (r > b + 30) & (h <= 25)
    skin_ratio = float(np.mean(skin_mask))
    if skin_ratio > 0.30:
        return False, f"Non-leaf warm tone / human skin object detected (ratio: {skin_ratio*100:.1f}%)."

    # 3. Chlorophyll & Foliage Mask
    green_foliage = (h >= 25) & (h <= 95) & (s >= 30) & (v >= 25)
    green_ratio = float(np.mean(green_foliage))

    yellow_foliage = (h >= 15) & (h < 25) & (s >= 40) & (g > b + 25) & (abs(r - g) < 25) & (r > 60)
    brown_foliage = (h >= 5) & (h < 25) & (s >= 35) & (g > b + 15) & (r - g < 25) & (g > 35)

    total_foliage_ratio = float(np.mean(green_foliage | yellow_foliage | brown_foliage))

    # Reject images that lack sufficient chlorophyll or total plant foliage
    if green_ratio < 0.08 and total_foliage_ratio < 0.22:
        return False, f"Image color distribution does not match plant foliage (Green: {green_ratio*100:.1f}%)."

    return True, "Valid Leaf"

ADVICE_DICT = {
    "Tomato___Bacterial_spot": {
        "stage": "Early to Mid Stage",
        "cause": "Caused by Xanthomonas bacteria, favored by high moisture and warm temperatures. Spreads rapidly by water splash.",
        "prevention": "Use disease-free seeds and transplants. Apply copper-based fungicides when symptoms first appear. Avoid overhead watering to reduce wetness on leaves.",
        "severity": "medium"
    },
    "Tomato___Early_blight": {
        "stage": "Mid to Late Stage",
        "cause": "Caused by the fungus Alternaria solani. Spores survive in plant debris and soil, spreading upward during warm, wet weather.",
        "prevention": "Prune lower leaves to improve air circulation. Use crop rotation and apply protective fungicides proactively.",
        "severity": "medium"
    },
    "Tomato___Late_blight": {
        "stage": "Rapid / Late Stage",
        "cause": "Caused by the water mold Phytophthora infestans. Thrives in cool, extremely wet environments and destroys plants in days.",
        "prevention": "Ensure excellent drainage. Destroy infected plants immediately. Apply systemic fungicides before prolonged rain.",
        "severity": "high"
    },
    "Tomato___Leaf_Mold": {
        "stage": "Mid Stage",
        "cause": "Fungal infection from Passalora fulva, primarily an issue in high-humidity environments like unventilated greenhouses.",
        "prevention": "Keep relative humidity below 85%. Provide adequate spacing for airflow and prune heavily.",
        "severity": "medium"
    },
    "Tomato___Septoria_leaf_spot": {
        "stage": "Early to Mid Stage",
        "cause": "Caused by the fungus Septoria lycopersici, typically attacking the oldest leaves first due to splashing rain or irrigation.",
        "prevention": "Apply mulch to prevent soil splashing. Remove affected lower leaves and use registered fungicides.",
        "severity": "medium"
    },
    "Tomato___Spider_mites Two-spotted_spider_mite": {
        "stage": "Any Stage",
        "cause": "Tiny arachnids that suck plant sap, thriving in hot, dry, and drought-stressed conditions.",
        "prevention": "Keep plants adequately watered. Introduce predatory mites or spray with horticultural oils or insecticidal soaps.",
        "severity": "low"
    },
    "Tomato___Target_Spot": {
        "stage": "Mid Stage",
        "cause": "Caused by the fungus Corynespora cassiicola, forming bullseye-like lesions under high relative humidity.",
        "prevention": "Improve airflow and avoid overcrowding. Do not leave plant debris over winter.",
        "severity": "medium"
    },
    "Tomato___Tomato_Yellow_Leaf_Curl_Virus": {
        "stage": "Systemic Viral Stage",
        "cause": "A devastating begomovirus transmitted exclusively by the silverleaf whitefly.",
        "prevention": "Use virus-resistant tomato varieties. Actively control whitefly populations using sticky traps and insecticidal sprays.",
        "severity": "high"
    },
    "Tomato___Tomato_mosaic_virus": {
        "stage": "Systemic Viral Stage",
        "cause": "A highly contagious virus transmitted by contact via contaminated hands, tools, or clothing.",
        "prevention": "Wash hands with soap before touching plants. Disinfect tools frequently and plant resistant cultivars.",
        "severity": "high"
    },
    "Tomato___healthy": {
        "stage": "Healthy",
        "cause": "Excellent growing conditions, proper watering, and good genetics.",
        "prevention": "Maintain your current routine! Ensure consistent watering, periodic fertilization, and regular scouting.",
        "severity": "none"
    }
}

def softmax(x):
    e_x = np.exp(x - np.max(x, axis=1, keepdims=True))
    return e_x / e_x.sum(axis=1, keepdims=True)

@app.post("/predict")
async def predict_disease(file: UploadFile = File(...)):
    # Reload model if loaded state changed
    if not model_loaded:
        load_model()

    if not model_loaded or ort_session is None or np is None:
        return {"error": "Model or required libraries failed to load on server."}

    try:
        contents = await file.read()
        image = Image.open(io.BytesIO(contents)).convert("RGB")
        
        # 1. Multi-Feature Botanical Leaf Image Validation
        is_leaf, leaf_msg = is_genuine_tomato_leaf(image)
        if not is_leaf:
            return {
                "prediction": "Invalid / Non-Tomato Image",
                "disease_key": "Invalid_Image",
                "confidence": 0.0,
                "severity": "unknown",
                "stage": "Not Applicable",
                "cause": "The uploaded photo does not contain a recognizable tomato plant leaf.",
                "prevention": "Please upload a clear, focused, well-lit photograph of a tomato leaf for disease diagnosis.",
                "class_id": -1
            }
        
        # Preprocess with ImageNet mean/std
        input_tensor = preprocess(image)
        
        # Inference using ONNX Runtime
        input_name = ort_session.get_inputs()[0].name
        outputs = ort_session.run(None, {input_name: input_tensor})
        probs = softmax(outputs[0])[0]
        
        sorted_probs = np.sort(probs)[::-1]
        top1_conf = float(sorted_probs[0])
        top2_conf = float(sorted_probs[1])
        margin_gap = top1_conf - top2_conf
        
        class_id_idx = int(np.argmax(probs))
        class_id = str(class_id_idx)
        
        # 2. Strict Confidence (>=82%) & Margin Gap (>=50%) Thresholding
        if top1_conf < 0.82 or margin_gap < 0.50:
            return {
                "prediction": "Invalid / Non-Tomato Image",
                "disease_key": "Uncertain_Prediction",
                "confidence": top1_conf,
                "severity": "unknown",
                "stage": "Uncertain Prediction",
                "cause": f"The AI model is uncertain about this photo (Confidence: {top1_conf*100:.1f}%, Margin: {margin_gap*100:.1f}%). It does not match a clear tomato leaf.",
                "prevention": "Please upload a clear, close-up photograph of a tomato leaf.",
                "class_id": -1
            }
        
        # Refresh LABELS if missing
        global LABELS
        if not LABELS and os.path.exists(LABELS_PATH):
            with open(LABELS_PATH, "r") as f:
                LABELS = json.load(f)
                
        disease_name = LABELS.get(class_id, "Unknown Disease")
        clean_name = disease_name.replace("Tomato___", "").replace("Two-spotted_spider_mite", "").replace("Two-spotted spider mite", "").replace("_", " ").strip()
        advice_info = ADVICE_DICT.get(disease_name, {
            "stage": "Unknown",
            "cause": "Could not be determined automatically.",
            "prevention": "Consult a local agricultural expert.",
            "severity": "unknown"
        })
        
        return {
            "prediction": clean_name,
            "disease_key": disease_name,
            "confidence": top1_conf,
            "severity": advice_info.get("severity", "unknown"),
            "stage": advice_info["stage"],
            "cause": advice_info["cause"],
            "prevention": advice_info["prevention"],
            "class_id": class_id_idx
        }
    except Exception as e:
        return {"error": str(e)}

@app.get("/")
def health_check():
    return {"status": "ok", "model_loaded": model_loaded}
