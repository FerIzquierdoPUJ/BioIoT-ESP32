// Restricted source-body harness: simulated Arduino types, not native ESP32 execution.
import vm from 'node:vm';
export function body(source,name) {
  const oneLine=source.match(new RegExp('^(?:const char\\*|bool|float) '+name+'\\([^;]*?\\)\\s*\\{([^\\n]*)\\}$','m'));
  if(oneLine) return oneLine[1];
  const match=source.match(new RegExp('^(?:inline\\s+)?(?:String|float|bool|void|uint8_t|const char\\*|Co2Reading|AnalogReading) '+name+'\\([^;]*?\\)\\s*\\{([\\s\\S]*?)^\\}', 'm'));
  if(!match) throw new Error('Function not found: '+name);
  return match[1];
}
const unwrap=v=>v && v.__jsonGet ? structuredClone(v.__jsonGet()) : v;
export function JsonDocument() {
  const root={value:null};
  function ref(get,set) {
    return new Proxy({},{
      get(_,key) {
        if(key==='__jsonGet') return get;
        if(key==='to') return kind=>{
          const v=kind?.includes('Array')?[]:{};
          set(v);return ref(get,set);
        };
        if(key==='as') return kind=>kind?.includes('Json')?ref(get,set):get();
        if(key==='is') return kind=>kind==='bool'?typeof get()==='boolean':
          kind?.includes('JsonObject')?get()!=null&&!Array.isArray(get())&&typeof get()==='object':
          typeof get()==='number';
        if(key==='isNull') return ()=>get()==null;
        if(key==='containsKey') return k=>get()!=null&&Object.hasOwn(get(),k);
        if(key==='add') return value=>{if(!Array.isArray(get()))set([]);get().push(unwrap(value));};
        if(key==='size') return ()=>get()?.length??Object.keys(get()??{}).length;
        if(key==='overflowed') return ()=>false;
        if(key==='set') return value=>set(unwrap(value));
        if(key==='toJSON') return get;
        if(key===Symbol.toPrimitive) return ()=>get();
        if(key===Symbol.iterator) return function*(){for(const value of get()??[])yield value;};
        return ref(()=>get()?.[key],v=>{if(get()==null)set({});get()[key]=v;});
      },
      set(_,key,value){if(get()==null)set({});get()[key]=unwrap(value);return true;}
    });
  }
  return ref(()=>root.value,v=>root.value=v);
}
export function convert(code) {
  return code.replace(/\/\/[^\n]*/g,'')
    .replace(/\b(?:const\s+)?(?:auto|float|bool|uint8_t|uint16_t|uint32_t|int64_t|int|size_t|String|JsonObject|JsonArray|Co2Calibration|Co2Reading|Calibration|O2Calibration)\s*&?\s*(\w+)\s*=/g,'let $1 =')
    .replace(/\bJsonDocument (\w+);/g,'let $1 = JsonDocument();')
    .replace(/\bCo2Reading (\w+);/g,'let $1 = Co2Reading();')
    .replace(/\bCo2Calibration (\w+);/g,'let $1 = Co2Calibration();')
    .replace(/\bString (\w+);/g,'let $1 = "";')
    .replace(/\b(?:const\s+)?char\s*\*\s*(\w+)\s*=/g,'let $1 =')
    .replace(/\.(to|as|is)<([^>]+)>\(\)/g,'.$1("$2")')
    .replace(/serializeJson\((.+), (\w+)\);/g,'$2 = serializeJson($1);')
    .replace(/\.c_str\(\)/g,'')
    .replace(/(\d+(?:\.\d*)?|\.\d+)f\b/g,'$1')
    .replace(/(\d+)UL\b/g,'$1')
    .replace(/(?<!sizeof)\((?:unsigned long|uint8_t|float)\)/g,'')
    .replace(/\bsizeof\(Co2Calibration\)/g,'20')
    .replace(/\bsizeof\(stored\)/g,'20')
    .replace(/\bsizeof\((?:float|uint32_t)\)/g,'4')
    .replace(/\bsizeof\(int64_t\)/g,'8')
    .replace(/&stored/g,'stored')
    .replace(/&co2Cal\[i\]/g,'co2Cal[i]')
    .replace(/\bCo2Calibration\(\)/g,'Co2Calibration()');
}
export function install(ctx,source,names) {
  for(const [name,args] of names) vm.runInContext('function '+name+'('+args+'){'+convert(body(source,name))+'}',ctx);
}
export const mathGlobals={
  isfinite:Number.isFinite, fabsf:Math.abs, expf:Math.exp, powf:Math.pow, NAN:NaN,nullptr:null,
  float:Number,int:Math.trunc, uint8_t:Number,bool:Boolean,
  String:(v,n)=>n===undefined?String(v):Number(v).toFixed(n),
  JsonDocument,serializeJson:v=>JSON.stringify(unwrap(v)),
  Co2Calibration:()=>({zero_point_v:.220,reaction_voltage_v:.030,a:0,b:0,mode:0,user:0,enabled:1,provisional:1}),
  Co2Reading:()=>({value:NaN,moduleVoltage:NaN,sensorVoltage:NaN,below:false,valid:false,quality:'uncalibrated'}),
  O2Calibration:()=>({gain:1,offset:0,reference:0,commandUtc:0,commandCount:0}),
  CALIBRATION_SCHEMA_VERSION:2,SEN0159_DC_GAIN:8.5,CO2_DIVIDER_RATIO:22/34,
  SEN0159_VENDOR_ZERO_POINT_V:.220,SEN0159_VENDOR_REACTION_V:.030,
  CO2_LOG_REFERENCE:2.602,CO2_REFERENCE_MAX_PPM:10000,O2_FILTER_SAMPLES:10
};
