import os
import json
import io
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
# Try local folder first, then maybe parent if structured differently
MODEL_PATH = os.path.join(BASE_DIR, "tomato_disease_detector.onnx")

if os.path.exists(LABELS_PATH):
    with open(LABELS_PATH, "r") as f:
        LABELS = json.load(f)
else:
    LABELS = {}

print("Loading local ONNX model...")
model_loaded = False
ort_session = None

try:
    if os.path.exists(MODEL_PATH) and ort is not None:
        ort_session = ort.InferenceSession(MODEL_PATH)
        model_loaded = True
        print("ONNX Model loaded successfully!")
    else:
        print(f"Model file not found at {MODEL_PATH} or onnxruntime missing")
except Exception as e:
    print(f"Error loading ONNX model: {e}")

# CLIP Validation is disabled by default to save memory/deployment size
# Enable only on systems with 2GB+ RAM
validator_loaded = False

def preprocess(image: Image.Image) -> np.ndarray:
    image = image.resize((128, 128), Image.Resampling.BILINEAR)
    img_array = np.array(image, dtype=np.float32) / 255.0
    if len(img_array.shape) != 3 or img_array.shape[2] != 3:
        raise ValueError("Image must be RGB")
    img_array = np.transpose(img_array, (2, 0, 1))
    return np.expand_dims(img_array, axis=0)

ADVICE_DICT = {
    "Tomato___Bacterial_spot": {
        "stage": "Early to Mid Stage",
        "cause": "Caused by Xanthomonas bacteria, favored by high moisture and warm temperatures. Spreads rapidly by water splash.",
        "prevention": "Use disease-free seeds and transplants. Apply copper-based fungicides when symptoms first appear. Avoid overhead watering to reduce wetness on leaves."
    },
    "Tomato___Early_blight": {
        "stage": "Mid to Late Stage",
        "cause": "Caused by the fungus Alternaria solani. Spores survive in plant debris and soil, spreading upward during warm, wet weather.",
        "prevention": "Prune lower leaves to improve air circulation. Use crop rotation and apply protective fungicides proactively."
    },
    "Tomato___Late_blight": {
        "stage": "Rapid / Late Stage",
        "cause": "Caused by the water mold Phytophthora infestans. Thrives in cool, extremely wet environments and destroys plants in days.",
        "prevention": "Ensure excellent drainage. Destroy infected plants immediately. Apply systemic fungicides before prolonged rain."
    },
    "Tomato___Leaf_Mold": {
        "stage": "Mid Stage",
        "cause": "Fungal infection from Passalora fulva, primarily an issue in high-humidity environments like unventilated greenhouses.",
        "prevention": "Keep relative humidity below 85%. Provide adequate spacing for airflow and prune heavily."
    },
    "Tomato___Septoria_leaf_spot": {
        "stage": "Early to Mid Stage",
        "cause": "Caused by the fungus Septoria lycopersici, typically attacking the oldest leaves first due to splashing rain or irrigation.",
        "prevention": "Apply mulch to prevent soil splashing. Remove affected lower leaves and use registered fungicides."
    },
    "Tomato___Spider_mites Two-spotted_spider_mite": {
        "stage": "Any Stage",
        "cause": "Tiny arachnids that suck plant sap, thriving in hot, dry, and drought-stressed conditions.",
        "prevention": "Keep plants adequately watered. Introduce predatory mites or spray with horticultural oils or completely covering insecticidal soaps."
    },
    "Tomato___Target_Spot": {
        "stage": "Mid Stage",
        "cause": "Caused by the fungus Corynespora cassiicola, forming bullseye-like lesions under high relative humidity.",
        "prevention": "Improve airflow and avoid overcrowding. Do not leave plant debris over winter."
    },
    "Tomato___Tomato_Yellow_Leaf_Curl_Virus": {
        "stage": "Systemic Viral Stage",
        "cause": "A devastating begomovirus transmitted exclusively by the silverleaf whitefly.",
        "prevention": "Use virus-resistant tomato varieties. Actively control whitefly populations using sticky traps and insecticidal sprays."
    },
    "Tomato___Tomato_mosaic_virus": {
        "stage": "Systemic Viral Stage",
        "cause": "A highly contagious virus transmitted by contact via contaminated hands, tools, or clothing.",
        "prevention": "Wash hands with soap before touching plants. Disinfect tools frequently and plant resistant cultivars."
    },
    "Tomato___healthy": {
        "stage": "Healthy",
        "cause": "Excellent growing conditions, proper watering, and good genetics.",
        "prevention": "Maintain your current routine! Ensure consistent watering, periodic fertilization, and regular scouting."
    }
}

def softmax(x):
    e_x = np.exp(x - np.max(x, axis=1, keepdims=True))
    return e_x / e_x.sum(axis=1, keepdims=True)

@app.post("/predict")
async def predict_disease(file: UploadFile = File(...)):
    if not model_loaded or ort_session is None or np is None:
        return {"error": "Model or required libraries failed to load on server."}

    try:
        contents = await file.read()
        image = Image.open(io.BytesIO(contents)).convert("RGB")
        
        # Preprocess
        input_tensor = preprocess(image)
        
        # Inference using ONNX Runtime
        input_name = ort_session.get_inputs()[0].name
        outputs = ort_session.run(None, {input_name: input_tensor})
        probs = softmax(outputs[0])
        
        class_id_idx = np.argmax(probs, axis=1)[0]
        confidence_val = float(probs[0][class_id_idx])
        class_id = str(class_id_idx)
        
        disease_name = LABELS.get(class_id, "Unknown Disease")
        clean_name = disease_name.replace("Tomato___", "").replace(" Two-spotted spider mite", "").replace("_", " ")
        advice_info = ADVICE_DICT.get(disease_name, {
            "stage": "Unknown",
            "cause": "Could not be determined automatically.",
            "prevention": "Consult a local agricultural expert."
        })
        
        return {
            "prediction": clean_name,
            "confidence": confidence_val,
            "stage": advice_info["stage"],
            "cause": advice_info["cause"],
            "prevention": advice_info["prevention"],
            "class_id": int(class_id)
        }
    except Exception as e:
        return {"error": str(e)}

@app.get("/")
def health_check():
    return {"status": "ok", "model_loaded": model_loaded}
