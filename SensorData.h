struct SensorData{
  float Temperature;
  float Humidity;
  float Pressure;
  String toString(){
    return String(Temperature) + " "+ String(Humidity) + " "+ String(Pressure);
  }
};
