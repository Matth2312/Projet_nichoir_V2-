#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import mariadb
from datetime import datetime
import paho.mqtt.client as mqtt

# ----------------------------- CONFIG MQTT -----------------------------
BROKER_IP   = "192.168.1.15" #je dois modif ca
BROKER_PORT = 1883

TOPIC_IMAGE = "nichoir/image"
TOPIC_BAT   = "nichoir/batterie"

# -----------------------------
# VARIABLE GLOBALE
# -----------------------------
# On stocke ici la dernière valeur de batterie recue
derniere_batterie = None 

# -----------------------------
# CONFIG DOSSIER IMAGES
# -----------------------------
HOME_DIR = os.path.expanduser("~")
SAVE_DIR = os.path.join(HOME_DIR, "Desktop", "images_reçues")
os.makedirs(SAVE_DIR, exist_ok=True)

# -----------------------------
# CONFIG BASE DE DONNÉES
# -----------------------------
DB_CONFIG = {
    "user": "mat",
    "password": "123456789",
    "host": "localhost",
    "database": "nichoir",
    "port": 3306
}

# -----------------------------
# FONCTION DE NETTOYAGE (SYNC)
# -----------------------------
def sync_database_with_folder():
    print("--- 🧹 Vérification de l'intégrité de la base de données ---")
    try:
        conn = mariadb.connect(**DB_CONFIG)
        cursor = conn.cursor()
        cursor.execute("SELECT id, chemin_fichier FROM images")
        rows = cursor.fetchall()
        deleted_count = 0

        for row in rows:
            image_id = row[0]
            chemin = row[1]
            if not os.path.exists(chemin):
                print(f"   ⚠️ Fichier introuvable : {chemin}")
                cursor.execute("DELETE FROM images WHERE id = ?", (image_id,))
                deleted_count += 1
        
        if deleted_count > 0:
            conn.commit()
            print(f"✅ Nettoyage terminé : {deleted_count} images supprimées.")
        else:
            print("✅ Tout est en ordre.")

        cursor.close()
        conn.close()
    except mariadb.Error as e:
        print("❌ ERREUR BDD :", e)
    print("----------------------------------------------------------")

def insert_image_record(chemin_fichier, niveau_batterie=None, commentaire=None):
    try:
        conn = mariadb.connect(**DB_CONFIG)
        cursor = conn.cursor()
        sql = """
            INSERT INTO images (chemin_fichier, date_capture, niveau_batterie, commentaire)
            VALUES (?, ?, ?, ?)
        """
        valeurs = (chemin_fichier, datetime.now(), niveau_batterie, commentaire)
        cursor.execute(sql, valeurs)
        conn.commit()
        print(f"   → Base mise à jour (ID = {cursor.lastrowid}, Batt = {niveau_batterie}V)")
        cursor.close()
        conn.close()
    except mariadb.Error as e:
        print("❌ ERREUR MariaDB :", e)

# -----------------------------
# CALLBACKS MQTT
# -----------------------------
def on_connect(client, userdata, flags, rc):
    print("Connecté au broker avec le code", rc)
    client.subscribe([(TOPIC_IMAGE, 0), (TOPIC_BAT, 0)]) # <--- Abonnement aux 2 topics
    print(f"Abonné à : {TOPIC_IMAGE} et {TOPIC_BAT}")

def on_message(client, userdata, msg):
    global derniere_batterie # On utilise la variable globale

    # 1. SI C'EST UN MESSAGE DE BATTERIE
    if msg.topic == TOPIC_BAT:
        try:
            payload_str = msg.payload.decode("utf-8")
            derniere_batterie = float(payload_str)
            print(f"🔋 Batterie reçue : {derniere_batterie} V")
        except Exception as e:
            print(f"❌ Erreur lecture batterie : {e}")

    # 2. SI C'EST UNE IMAGE
    elif msg.topic == TOPIC_IMAGE:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"image_{timestamp}.jpg"
        filepath = os.path.join(SAVE_DIR, filename)

        try:
            with open(filepath, "wb") as f:
                f.write(msg.payload)
            print(f"[{timestamp}] Image reçue → {filepath}")
            
            # On insère en base avec la dernière valeur de batterie connue
            full_path = os.path.abspath(filepath)
            insert_image_record(
                chemin_fichier=full_path,
                niveau_batterie=derniere_batterie, # <--- Utilisation ici
                commentaire="Image auto"
            )
            
        except Exception as e:
            print("❌ ERREUR écriture fichier :", e)

# -----------------------------
# LANCEMENT PRINCIPAL
# -----------------------------
if __name__ == "__main__":
    sync_database_with_folder()
    
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    print("Connexion au broker MQTT...")
    try:
        client.connect(BROKER_IP, BROKER_PORT, keepalive=60)
        client.loop_forever()
    except Exception as e:
        print(f"❌ Impossible de se connecter : {e}")