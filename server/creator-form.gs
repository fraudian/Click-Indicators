/**
 * Builds the Click Indicators creator-programme Google Form.
 *
 * There is no way to import questions into Google Forms, so this creates them via Apps Script
 * instead of you typing fourteen fields by hand.
 *
 * HOW TO RUN
 *   1. Go to script.google.com and start a new project
 *   2. Delete whatever is in the editor, paste this whole file in
 *   3. Put your TikTok handle in TIKTOK below
 *   4. Press Run. Approve the permission prompt - it is asking to create a form in your Drive
 *   5. The form's public link and edit link are printed in the Execution log at the bottom
 *
 * Responses land in the form itself; use the Responses tab, or link a Sheet from there if you
 * would rather have rows.
 */

// ---- put your handle here ----------------------------------------------------------------
var TIKTOK = '@yourhandle';        // shown as the example of the style you are after
var SITE   = 'clickindicatorsmod.com';
var DISCORD = 'discord.gg/FncjJJNcES';
// -------------------------------------------------------------------------------------------

function createCreatorForm() {
  var form = FormApp.create('Click Indicators - free key for creators');

  form.setDescription(
    'Make videos about Click Indicators and you get it free.\n\n' +
    'The kind of thing I am after is what is on ' + TIKTOK + ' - short clips where the guide is ' +
    'visibly doing something, not a talking-head review. Gameplay with the cues on screen.\n\n' +
    'I read every one of these myself. Small accounts are fine - a 400 follower account that ' +
    'actually posts is worth more to me than a big one that will not.'
  );

  form.setCollectEmail(false);      // asked for explicitly below, so it matches their account
  form.setProgressBar(false);
  form.setLimitOneResponsePerUser(false);   // no Google account required to apply

  // --- who you are -------------------------------------------------------------------------
  form.addSectionHeaderItem().setTitle('Who you are');

  form.addTextItem()
    .setTitle('Discord username')
    .setHelpText('How I get back to you, and how you get the creator role. Join ' + DISCORD +
                 ' first if you have not.')
    .setRequired(true);

  form.addTextItem()
    .setTitle('Email address')
    .setHelpText('This has to be the address you use for your ' + SITE + ' account. Make the ' +
                 'account first if you do not have one - the free key gets applied to it. If it ' +
                 'does not match, I cannot give you anything.')
    .setRequired(true);

  // --- your channel ------------------------------------------------------------------------
  form.addSectionHeaderItem().setTitle('Your channel');

  form.addCheckboxItem()
    .setTitle('Where do you post?')
    .setChoiceValues(['TikTok', 'YouTube', 'YouTube Shorts', 'Instagram Reels', 'Twitch', 'Somewhere else'])
    .setRequired(true);

  form.addTextItem()
    .setTitle('Link to your profile')
    .setHelpText('The main one. Paste the full link.')
    .setRequired(true);

  form.addMultipleChoiceItem()
    .setTitle('Roughly how many followers/subs?')
    .setChoiceValues(['Under 500', '500 - 2,000', '2,000 - 10,000', '10,000 - 50,000', 'Over 50,000'])
    .setHelpText('Honest answer please. This is not a cutoff, it just tells me what to expect.')
    .setRequired(true);

  form.addParagraphTextItem()
    .setTitle('Link one or two recent videos that show your usual style')
    .setHelpText('This is the part I actually look at. Recent and typical beats your single best ' +
                 'video from a year ago.')
    .setRequired(true);

  // --- the deal ----------------------------------------------------------------------------
  form.addSectionHeaderItem().setTitle('The deal');

  form.addMultipleChoiceItem()
    .setTitle('Do you already own Click Indicators?')
    .setChoiceValues(['No, not yet', 'Yes, I bought it'])
    .setHelpText('If you already bought it and get accepted, say so in the Discord and I will ' +
                 'sort you out rather than you paying twice.')
    .setRequired(true);

  form.addParagraphTextItem()
    .setTitle('What would you actually make with it?')
    .setHelpText('A sentence is fine. "Progress on my hardest level with the guide on" is a real ' +
                 'answer. I am mostly checking you have a plan.')
    .setRequired(true);

  form.addCheckboxItem()
    .setTitle('Agreed?')
    .setChoiceValues([
      'I will post at least one video within 14 days of getting the key',
      'I will say it is Click Indicators and link ' + SITE,
      'I will not present it as a bot - it shows you when to click, it does not play for you'
    ])
    .setRequired(true);

  form.addParagraphTextItem()
    .setTitle('Anything else?')
    .setHelpText('Optional.');

  Logger.log('PUBLIC LINK (share this): ' + form.getPublishedUrl());
  Logger.log('EDIT LINK  (keep this)  : ' + form.getEditUrl());
  return form.getPublishedUrl();
}
