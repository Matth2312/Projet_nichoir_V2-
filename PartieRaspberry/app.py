from flask import Flask, render_template, send_file, abort
import mysql.connector
import os

app = Flask(__name__)

# Configuration de la base de données
db_config = {
    "user": "mat",
    "password": "123456789",
    "host": "localhost",
    "database": "nichoir"

}

def get_db_connection():
    conn = mysql.connector.connect(**db_config)
    return conn

@app.route('/')
def index():
    conn = get_db_connection()
    cursor = conn.cursor(dictionary=True) # dictionary=True permet d'accéder aux colonnes par leur nom
    # On récupère les images, triées de la plus récente à la plus ancienne
    cursor.execute("SELECT * FROM images ORDER BY date_capture DESC")
    photos = cursor.fetchall()
    conn.close()
    return render_template('index.html', photos=photos)

# Route spéciale pour servir l'image depuis le disque dur
@app.route('/get_image/<int:image_id>')
def get_image(image_id):
    conn = get_db_connection()
    cursor = conn.cursor(dictionary=True)
    # On cherche le chemin du fichier pour cet ID
    cursor.execute("SELECT chemin_fichier FROM images WHERE id = %s", (image_id,))
    result = cursor.fetchone()
    conn.close()

    if result:
        chemin_absolu = result['chemin_fichier']
        # Vérification si le fichier existe vraiment sur le Pi
        if os.path.exists(chemin_absolu):
            return send_file(chemin_absolu, mimetype='image/jpeg')
        else:
            return "Fichier introuvable sur le disque", 404
    else:
        abort(404)

if __name__ == '__main__':
    # host='0.0.0.0' permet d'y accéder depuis un autre PC sur le réseau
    app.run(debug=True, host='0.0.0.0', port=5000)