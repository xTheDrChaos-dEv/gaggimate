import { Chart } from 'chart.js';
import { ChartComponent } from '../../components/Chart.jsx';
import { t } from '@lingui/core/macro';

// Helper function to get phase name from shot data (v5+) or fallback to profile/generic
function getPhaseName(shot, phaseNumber) {
  // For v5+ shots, check if we have phase transition data
  if (shot.phaseTransitions && shot.phaseTransitions.length > 0) {
    const transition = shot.phaseTransitions.find(t => t.phaseNumber === phaseNumber);
    if (transition && transition.phaseName) {
      return transition.phaseName;
    }
  }

  // Fallback to no label for v4 shots or missing data
  return '';
}

function getChartData(shot) {
  // Build sample point arrays once (numeric x for linear scale)
  const data = shot.samples;
  const ct = [];
  const tt = [];
  const cp = [];
  const tp = [];
  const fl = [];
  const pf = [];
  const tf = [];
  const v = [];

  // Process all samples to build data arrays
  for (let i = 0; i < data.length; i++) {
    const s = data[i];
    const x = s.t / 1000.0; // Convert time to seconds for chart display

    ct.push({ x, y: s.ct });
    tt.push({ x, y: s.tt });
    cp.push({ x, y: s.cp });
    tp.push({ x, y: s.tp });
    fl.push({ x, y: s.fl });
    pf.push({ x, y: s.pf });
    tf.push({ x, y: s.tf });
    v.push({ x, y: s.v });
  }

  // For v5+ files, use phase transitions from header
  let phaseTransitions = [];

  if (shot.version >= 5 && shot.phaseTransitions) {
    // Use phase transitions directly from header
    phaseTransitions = shot.phaseTransitions.map(t => ({
      time: (t.sampleIndex * (shot.sampleInterval || 250)) / 1000.0,
      phaseNumber: t.phaseNumber,
      phaseDisplayNumber: t.phaseNumber + 1,
      phaseName: t.phaseName,
    }));
  }
  // No phase tracking for versions before v5
  const tempValues = ct.map(i => i.y).concat(tt.map(i => i.y));
  const timeValues = ct.map(i => i.x);
  const minTemp = Math.floor(Math.min(...tempValues));
  const maxTemp = Math.ceil(Math.max(...tempValues));
  const minX = Math.min(...timeValues);
  const maxX = Math.max(...timeValues);
  const padding = maxTemp - minTemp > 10 ? 2 : 5;

  // Series colours follow the dashboard's live chart (temperature red, pressure teal, pump flow sage, weight
  // gold). The history card sits on the page background, so dark themes get the light tones and light themes
  // the deep ones.
  const darkPage = ['dark', 'coffee', 'olive', 'purple'].includes(document.documentElement.dataset.theme);
  const series = darkPage
    ? {
        temp: '#FF6B5C',
        tempTarget: '#FFB3A8',
        pressure: '#70D6C2',
        flow: '#B3D5A5',
        puck: '#7FB069',
        weight: '#F4BD64',
      }
    : {
        temp: '#C2331F',
        tempTarget: '#8F2A2A',
        pressure: '#0B6B5D',
        flow: '#527341',
        puck: '#2F4A22',
        weight: '#925408',
      };

  // Check if weight data has any non-zero values
  const hasWeight = v.some(point => point.y > 0);

  // Create phase annotations
  const phaseAnnotations = {};
  const isSmall = window.innerWidth < 640;

  // Add shot start marker
  if (data.length > 0) {
    const shotStartTime = data[0].t / 1000.0; // First sample time in seconds
    const startPhaseName = getPhaseName(shot, 0); // Get first phase name
    phaseAnnotations['shot_start'] = {
      type: 'line',
      xMin: shotStartTime,
      xMax: shotStartTime,
      borderColor: '#6B7280', // Gray color to match other phase lines
      borderWidth: 1,
      label: {
        display: true,
        content: startPhaseName,
        rotation: -90,
        position: 'end',
        xAdjust: -5,
        yAdjust: 0,
        padding: { x: 6, y: 0 },
        color: 'rgb(255,255,255)',
        backgroundColor: 'rgba(22,33,50,0.75)',
        textAlign: 'start',
        font: {
          size: isSmall ? 9 : 11,
          weight: 500,
        },
        clip: false,
      },
    };
  }

  phaseTransitions.forEach((transition, index) => {
    // Skip the first transition since we handle it with shot_start marker
    if (index === 0) return;

    const phaseNumber = transition.phaseDisplayNumber || transition.phaseNumber + 1;
    phaseAnnotations[`phase_line_${index}`] = {
      type: 'line',
      xMin: transition.time,
      xMax: transition.time,
      borderColor: '#6B7280', // Gray color to match OverviewChart
      borderWidth: 1,
      label: {
        display: true,
        content: transition.phaseName || t`Phase ${phaseNumber}`,
        rotation: -90,
        position: 'end',
        xAdjust: -10,
        yAdjust: 0,
        padding: { x: 6, y: 0 },
        color: 'rgb(255,255,255)',
        backgroundColor: 'rgba(22,33,50,0.75)',
        textAlign: 'start',
        font: {
          size: isSmall ? 9 : 11,
          weight: 500,
        },
        clip: false,
      },
    };
  });
  return {
    type: 'line',
    data: {
      datasets: [
        {
          label: t`Current Temperature`,
          borderColor: series.temp,
          pointStyle: false,
          data: ct,
        },
        {
          label: t`Target Temperature`,
          fill: true,
          borderColor: series.tempTarget,
          borderDash: [6, 6],
          pointStyle: false,
          data: tt,
        },
        {
          label: t`Current Pressure`,
          borderColor: series.pressure,
          pointStyle: false,
          yAxisID: 'y1',
          data: cp,
        },
        {
          label: t`Target Pressure`,
          fill: true,
          borderColor: series.pressure,
          borderDash: [6, 6],
          pointStyle: false,
          yAxisID: 'y1',
          data: tp,
        },
        {
          label: t`Current Pump Flow`,
          borderColor: series.flow,
          pointStyle: false,
          yAxisID: 'y1',
          data: fl,
        },
        {
          label: t`Current Puck Flow`,
          borderColor: series.puck,
          pointStyle: false,
          yAxisID: 'y1',
          data: pf,
        },
        {
          label: t`Target Pump Flow`,
          borderColor: series.flow,
          borderDash: [6, 6],
          pointStyle: false,
          yAxisID: 'y1',
          data: tf,
        },
        // Only include weight datasets if they have non-zero values
        ...(hasWeight
          ? [
              {
                label: t`Weight`,
                borderColor: series.weight,
                pointStyle: false,
                yAxisID: 'y2',
                data: v,
              },
            ]
          : []),
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      parsing: false, // We already provide x/y objects
      spanGaps: true,
      plugins: {
        legend: {
          position: 'top',
          display: true,
          labels: {
            usePointStyle: true,
            pointStyle: 'line',
            pointStyleWidth: 20,
            padding: 8,
            font: {
              size: window.innerWidth < 640 ? 10 : 12,
            },
            generateLabels(chart) {
              const original = Chart.defaults.plugins.legend.labels.generateLabels;
              const labels = original.call(this, chart);

              labels.forEach((label, index) => {
                const dataset = chart.data.datasets[index];
                label.lineWidth = 3;
                if (dataset.borderDash && dataset.borderDash.length > 0) {
                  label.lineDash = dataset.borderDash;
                }
              });

              return labels;
            },
          },
        },
        title: {
          display: false,
        },
        annotation: {
          annotations: phaseAnnotations,
        },
      },
      animation: false,
      scales: {
        x: {
          min: minX,
          max: maxX,
          type: 'linear',
          title: { display: true, text: t`Time (s)` },
          ticks: { font: { size: window.innerWidth < 640 ? 10 : 12 } },
        },
        y: {
          type: 'linear',
          ticks: {
            callback: value => `${value.toFixed()} °C`,
            font: {
              size: window.innerWidth < 640 ? 10 : 12,
            },
          },
          min: Math.max(minTemp - padding, 0),
          max: maxTemp + padding,
        },
        y1: {
          type: 'linear',
          min: 0,
          max: 16,
          position: 'right',
          ticks: {
            callback: value => `${value.toFixed()} bar / ml/s`,
            font: {
              size: window.innerWidth < 640 ? 10 : 12,
            },
          },
          grid: { drawOnChartArea: false },
        },
        ...(hasWeight
          ? {
              y2: {
                type: 'linear',
                min: 0,
                position: 'right',
                offset: true,
                ticks: {
                  callback: value => `${value.toFixed()} g`,
                  font: {
                    size: window.innerWidth < 640 ? 10 : 12,
                  },
                },
                grid: { drawOnChartArea: false },
              },
            }
          : {}),
      },
    },
  };
}

export function HistoryChart({ shot }) {
  const chartData = getChartData(shot);

  return (
    <ChartComponent
      className='min-h-[350px] flex-1'
      chartClassName='w-full min-h-[350px]'
      data={chartData}
    />
  );
}
