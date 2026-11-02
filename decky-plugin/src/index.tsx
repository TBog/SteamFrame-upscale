import { PanelSection, PanelSectionRow, SliderField, staticClasses } from "@decky/ui";
import { callable, definePlugin } from "@decky/api";
import { useEffect, useState } from "react";
import { FaEye } from "react-icons/fa";

// Mirrors sf::Config in include/denoise.hpp / config.ini -- keep names and defaults here in sync
// with that struct if its fields ever change. The backend (main.py) is the only thing that
// touches config.ini; this component just reflects/edits it.
interface Params {
  temporal_alpha: number;
  motion_threshold: number;
  sharpness_weight: number;
  noise_floor: number;
}

const DEFAULTS: Params = {
  temporal_alpha: 0.15,
  motion_threshold: 0.08,
  sharpness_weight: 0.8,
  noise_floor: 0.02,
};

const getConfig = callable<[], Params>("get_config");
const setParam = callable<[key: string, value: number], number>("set_param");

function ParamSlider({
  param,
  label,
  description,
  value,
  onChange,
}: {
  param: keyof Params;
  label: string;
  description: string;
  value: number;
  onChange: (param: keyof Params, value: number) => void;
}) {
  return (
    <PanelSectionRow>
      <SliderField
        label={label}
        description={description}
        value={value}
        min={0}
        max={1}
        step={0.01}
        showValue
        onChange={(v) => onChange(param, v)}
      />
    </PanelSectionRow>
  );
}

function Content() {
  const [params, setParams] = useState<Params>(DEFAULTS);
  const [loaded, setLoaded] = useState(false);

  useEffect(() => {
    getConfig().then((cfg) => {
      setParams(cfg);
      setLoaded(true);
    });
  }, []);

  // Update the slider immediately for responsiveness; the write lands on disk right away too,
  // and the layer picks it up on its own within a frame or two (see SETUP.md) -- there's nothing
  // else to wait on here.
  const onChange = (param: keyof Params, value: number) => {
    setParams((p) => ({ ...p, [param]: value }));
    setParam(param, value);
  };

  if (!loaded) {
    return (
      <PanelSection title="Passthrough Denoise">
        <PanelSectionRow>Loading config.ini...</PanelSectionRow>
      </PanelSection>
    );
  }

  return (
    <PanelSection title="Passthrough Denoise">
      <ParamSlider
        param="temporal_alpha"
        label="Temporal smoothing"
        description="Weight of the current frame on static pixels. Lower = smoother but more lag; raise if trailing/ghosting appears."
        value={params.temporal_alpha}
        onChange={onChange}
      />
      <ParamSlider
        param="motion_threshold"
        label="Motion threshold"
        description="Luma change above which temporal blending is bypassed. Lower if fast motion still ghosts; raise if static noise flickers."
        value={params.motion_threshold}
        onChange={onChange}
      />
      <ParamSlider
        param="sharpness_weight"
        label="Sharpening"
        description="Max CAS sharpening intensity. Lower if sensor grain looks crunchy/amplified; raise if the image looks too soft."
        value={params.sharpness_weight}
        onChange={onChange}
      />
      <ParamSlider
        param="noise_floor"
        label="Noise floor"
        description="Contrast below this is treated as noise and not sharpened. Raise if remaining ISO grain is getting sharpened."
        value={params.noise_floor}
        onChange={onChange}
      />
    </PanelSection>
  );
}

export default definePlugin(() => {
  return {
    name: "SteamFrame Passthrough Denoise",
    titleView: <div className={staticClasses.Title}>Passthrough Denoise</div>,
    content: <Content />,
    icon: <FaEye />,
  };
});
