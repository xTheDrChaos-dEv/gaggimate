import beans from '../assets/bean-tile.svg?raw';
import floral from '../assets/floral-tile.svg?raw';

// The page background that mirrors the round screen's background style: the same coffee-bean or floral tile,
// repeated behind all content and coloured by the current theme (style.css, .gm-backdrop). Which one shows, and
// whether it drifts, follows data-bg on <html>, which comes from the machine (utils/themeManager.js).
const TILES = [
  { name: 'beans', size: 240, markup: beans },
  { name: 'floral', size: 320, markup: floral },
];

export function Backdrop() {
  return (
    <div className='gm-backdrop' aria-hidden='true'>
      {TILES.map(tile => (
        <svg key={tile.name} className={`gm-backdrop-${tile.name}`} xmlns='http://www.w3.org/2000/svg'>
          <defs>
            <pattern
              id={`gm-tile-${tile.name}`}
              width={tile.size}
              height={tile.size}
              patternUnits='userSpaceOnUse'
              // The markup is one of the two tile files bundled above, never user input.
              // eslint-disable-next-line react/no-danger
              dangerouslySetInnerHTML={{ __html: tile.markup }}
            />
          </defs>
          <rect width='100%' height='100%' fill={`url(#gm-tile-${tile.name})`} stroke='none' />
        </svg>
      ))}
    </div>
  );
}
