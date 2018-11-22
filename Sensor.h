struct Sensor{
  String SensorIP;
  String SensorMAC;
  String SensorFunction;
  String FunctionVersion;
  bool isInitialised(){
    return SensorMAC != "";
  }
};

struct SensorInfo{
  String Function;
  String Version;
};

