import PropTypes from 'prop-types';
import { useLingui } from '@lingui/react/macro';
import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import { faPlus } from '@fortawesome/free-solid-svg-icons/faPlus';
import { faTrashCan } from '@fortawesome/free-solid-svg-icons/faTrashCan';
import {
  DEFAULT_BACKGROUND,
  MAX_NAME_LENGTH,
  MAX_PEOPLE,
  backgroundStyles,
  displayThemes,
  initialOf,
} from '../../utils/people.js';

// People who share the machine. With two or more, waking it by touch asks who is brewing, and each person gets
// their own display theme and brew defaults back.
export function UsersCard({ formData, setField, onChange }) {
  const { t } = useLingui();
  const people = Array.isArray(formData.users) ? formData.users : [];
  const active = Math.min(
    Math.max(Number(formData.activeUser ?? 0), 0),
    Math.max(people.length - 1, 0),
  );
  const themes = displayThemes();
  const backgrounds = backgroundStyles();

  const setPeople = next => setField('users', next);
  const update = (index, patch) => {
    setPeople(people.map((person, i) => (i === index ? { ...person, ...patch } : person)));
    // The live display theme and background are the active person's, so keep the controls in step.
    if (index === active && patch.theme !== undefined) setField('themeMode', patch.theme);
    if (index === active && patch.bg !== undefined) setField('backgroundStyle', patch.bg);
  };
  const add = () =>
    setPeople([...people, { name: '', theme: 0, bg: DEFAULT_BACKGROUND, private: [] }]);
  const remove = index => {
    setPeople(people.filter((_, i) => i !== index));
    setField('activeUser', index === active ? 0 : index < active ? active - 1 : active);
  };
  const makeActive = index => {
    setField('activeUser', index);
    setField('themeMode', people[index].theme);
    setField('backgroundStyle', people[index].bg ?? DEFAULT_BACKGROUND);
  };

  return (
    <div className='space-y-4'>
      <p className='text-base-content/70 text-sm'>
        {t`Each person keeps their own display theme and background, selected and favorite profiles, and brew, steam, water and grind targets.`}
      </p>
      {people.map((person, index) => (
        <div key={index} className='bg-base-200/60 space-y-2 rounded-xl p-2'>
          {/* Two rows, so the name field keeps its width on a phone. */}
          <div className='flex items-center gap-2'>
            <span
              className='bg-primary/15 text-primary flex h-9 w-9 shrink-0 items-center justify-center rounded-full font-bold'
              aria-hidden='true'
            >
              {initialOf(person.name)}
            </span>
            <input
              type='text'
              className='input input-bordered min-w-0 flex-1'
              value={person.name}
              maxLength={MAX_NAME_LENGTH}
              placeholder={t`Name`}
              aria-label={t`Name of person ${index + 1}`}
              onInput={e => update(index, { name: e.target.value })}
            />
            <button
              type='button'
              className='btn btn-ghost btn-sm shrink-0'
              onClick={() => remove(index)}
              aria-label={t`Remove ${person.name || t`this person`}`}
            >
              <FontAwesomeIcon icon={faTrashCan} />
            </button>
          </div>
          <div className='flex items-center gap-2'>
            <select
              className='select select-bordered min-w-0 flex-1'
              value={person.theme}
              aria-label={t`Display theme for ${person.name || t`this person`}`}
              onChange={e => update(index, { theme: Number(e.target.value) })}
            >
              {themes.map(theme => (
                <option key={theme.value} value={theme.value}>
                  {theme.label}
                </option>
              ))}
            </select>
            <select
              className='select select-bordered min-w-0 flex-1'
              value={person.bg ?? DEFAULT_BACKGROUND}
              aria-label={t`Background for ${person.name || t`this person`}`}
              onChange={e => update(index, { bg: Number(e.target.value) })}
            >
              {backgrounds.map(style => (
                <option key={style.value} value={style.value}>
                  {style.label}
                </option>
              ))}
            </select>
          </div>
          <div className='flex items-center'>
            <label className='flex shrink-0 cursor-pointer items-center gap-1 text-xs'>
              <input
                type='radio'
                className='radio radio-sm'
                name='activeUserChoice'
                checked={index === active}
                onChange={() => makeActive(index)}
              />
              {t`Brewing now`}
            </label>
          </div>
        </div>
      ))}
      {people.length < MAX_PEOPLE && (
        <button type='button' className='btn btn-outline btn-sm' onClick={add}>
          <FontAwesomeIcon icon={faPlus} />
          {t`Add person`}
        </button>
      )}
      <label className='flex cursor-pointer items-center gap-3'>
        <input
          id='usersAskOnWake'
          name='usersAskOnWake'
          value='usersAskOnWake'
          type='checkbox'
          className='toggle toggle-primary'
          checked={!!formData.usersAskOnWake}
          onChange={onChange('usersAskOnWake')}
        />
        <span className='text-sm'>{t`Ask who's brewing when the machine is woken by touch`}</span>
      </label>
      {people.length < 2 && (
        <p className='text-base-content/60 text-xs'>{t`Add at least two people for the question to appear.`}</p>
      )}
    </div>
  );
}

UsersCard.propTypes = {
  formData: PropTypes.object.isRequired,
  setField: PropTypes.func.isRequired,
  onChange: PropTypes.func.isRequired,
};
